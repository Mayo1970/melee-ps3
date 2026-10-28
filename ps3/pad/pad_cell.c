// DS3 pads through PSL1GHT's ioPad. See pad_cell.h.
#include "pad_cell.h"

#include <string.h>

#include <io/pad.h>

#include "../os/diag.h"
#include "../os/sync.h"

#define PORTS 7
#define MAX_READ 4

static PsMutex lock = PS_MUTEX_INIT;
static padData last[PORTS];
static bool pressure_on[PORTS];
static int read_port[MAX_READ] = { -1, -1, -1, -1 };
static int chan_port[MAX_READ] = { -1, -1, -1, -1 };

void ps3_pad_init(void)
{
    ioPadInit(PORTS);
}

static unsigned char digital(unsigned pressed)
{
    return pressed ? 255 : 0;
}

static void convert(const padData* d, Ps3Pad* p)
{
    // With pressure mode on, the pad sends the pressures after the sticks.
    bool pressures = d->len >= 20;

    p->connected = true;
    p->buttons = (d->BTN_LEFT ? PS3_PAD_LEFT : 0) | (d->BTN_RIGHT ? PS3_PAD_RIGHT : 0) |
                 (d->BTN_UP ? PS3_PAD_UP : 0) | (d->BTN_DOWN ? PS3_PAD_DOWN : 0) |
                 (d->BTN_CROSS ? PS3_PAD_CROSS : 0) | (d->BTN_CIRCLE ? PS3_PAD_CIRCLE : 0) |
                 (d->BTN_SQUARE ? PS3_PAD_SQUARE : 0) | (d->BTN_TRIANGLE ? PS3_PAD_TRIANGLE : 0) |
                 (d->BTN_L1 ? PS3_PAD_L1 : 0) | (d->BTN_R1 ? PS3_PAD_R1 : 0) | (d->BTN_L2 ? PS3_PAD_L2 : 0) |
                 (d->BTN_R2 ? PS3_PAD_R2 : 0) | (d->BTN_START ? PS3_PAD_START : 0) |
                 (d->BTN_SELECT ? PS3_PAD_SELECT : 0);
    p->lx = (unsigned char) d->ANA_L_H;
    p->ly = (unsigned char) d->ANA_L_V;
    p->rx = (unsigned char) d->ANA_R_H;
    p->ry = (unsigned char) d->ANA_R_V;
    p->l2 = pressures ? (unsigned char) d->PRE_L2 : digital(d->BTN_L2);
    p->r2 = pressures ? (unsigned char) d->PRE_R2 : digital(d->BTN_R2);
    p->cross = pressures ? (unsigned char) d->PRE_CROSS : digital(d->BTN_CROSS);
    p->square = pressures ? (unsigned char) d->PRE_SQUARE : digital(d->BTN_SQUARE);
}

void ps3_pad_read(Ps3Pad* pads, int n)
{
    padInfo info;
    int port, c, used = 0;

    memset(pads, 0, sizeof(*pads) * (size_t) n);
    ps_mutex_lock(&lock);
    if (ioPadGetInfo(&info) == 0) {
        // A GameCube channel is a fixed port: a pad keeps its channel while others come and go.
        for (c = 0; c < MAX_READ; c++) {
            if (chan_port[c] >= 0 && !info.status[chan_port[c]]) {
                chan_port[c] = -1;
            }
        }
        for (port = 0; port < PORTS; port++) {
            padData d;

            if (!info.status[port]) {
                pressure_on[port] = false;
                memset(&last[port], 0, sizeof(last[port]));
                continue;
            }
            if (!pressure_on[port]) {
                ioPadSetPressMode((u32) port, 1);
                pressure_on[port] = true;
            }
            // len 0: nothing changed since the last read.
            if (ioPadGetData((u32) port, &d) == 0 && d.len > 0) {
                last[port] = d;
            }
            for (c = 0; c < MAX_READ && chan_port[c] != port; c++) {
            }
            if (c == MAX_READ) {
                for (c = 0; c < MAX_READ && chan_port[c] >= 0; c++) {
                }
                if (c < MAX_READ) {
                    chan_port[c] = port;
                }
            }
        }
    }
    for (c = 0; c < MAX_READ; c++) {
        read_port[c] = c < n ? chan_port[c] : -1;
        if (read_port[c] >= 0) {
            convert(&last[read_port[c]], &pads[c]);
            used++;
        }
    }
    ps3_diag.pads = (unsigned) used;
    ps3_diag.pad0_buttons = n > 0 ? pads[0].buttons : 0;
    ps_mutex_unlock(&lock);
}

void ps3_pad_rumble(int index, bool on)
{
    padActParam act;
    int port;

    ps_mutex_lock(&lock);
    port = index >= 0 && index < MAX_READ ? read_port[index] : -1;
    if (port >= 0) {
        memset(&act, 0, sizeof(act));
        act.small_motor = on ? 1 : 0;
        act.large_motor = on ? 160 : 0;
        ioPadSetActDirect((u32) port, &act);
    }
    ps_mutex_unlock(&lock);
}
