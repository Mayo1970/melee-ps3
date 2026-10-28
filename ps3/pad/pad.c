// The SDK's PAD calls over DS3 pads (pad_cell.c). Cross, Square, Circle and Triangle are A, B, X and Y as they
// lie on the GameCube pad; R1 is Z, L2 and R2 the analog L and R, the right stick the C stick.
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include "../os/diag.h"
#include "pad_cell.h"

// L and R also click at the end of their travel.
#define TRIGGER_CLICK 230

static const struct {
    unsigned ps3;
    u16 gc;
} buttons[] = {
    { PS3_PAD_LEFT, PAD_BUTTON_LEFT },   { PS3_PAD_RIGHT, PAD_BUTTON_RIGHT }, { PS3_PAD_DOWN, PAD_BUTTON_DOWN },
    { PS3_PAD_UP, PAD_BUTTON_UP },       { PS3_PAD_R1, PAD_TRIGGER_Z },       { PS3_PAD_CROSS, PAD_BUTTON_A },
    { PS3_PAD_SQUARE, PAD_BUTTON_B },    { PS3_PAD_CIRCLE, PAD_BUTTON_X },    { PS3_PAD_TRIANGLE, PAD_BUTTON_Y },
    { PS3_PAD_START, PAD_BUTTON_START },
};

static s8 axis(unsigned char v, bool flip)
{
    int a = flip ? 128 - (int) v : (int) v - 128;

    return (s8) (a > 127 ? 127 : a);
}

BOOL PADInit(void)
{
    static bool done;

    if (!done) {
        done = true;
        ps3_pad_init();
    }
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    Ps3Pad pads[PAD_MAX_CONTROLLERS];
    u32 connected = 0;
    int chan;
    size_t i;

    ps3_diag.pad_reads++;
    ps3_pad_read(pads, PAD_MAX_CONTROLLERS);
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        PADStatus* s = &status[chan];
        const Ps3Pad* p = &pads[chan];

        memset(s, 0, sizeof(*s));
        if (!p->connected) {
            s->err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            if (p->buttons & buttons[i].ps3) {
                s->button |= buttons[i].gc;
            }
        }
        if (p->l2 >= TRIGGER_CLICK) {
            s->button |= PAD_TRIGGER_L;
        }
        if (p->r2 >= TRIGGER_CLICK) {
            s->button |= PAD_TRIGGER_R;
        }
        // The PS3 sticks grow downwards; the GameCube's grow upwards.
        s->stickX = axis(p->lx, false);
        s->stickY = axis(p->ly, true);
        s->substickX = axis(p->rx, false);
        s->substickY = axis(p->ry, true);
        s->triggerLeft = p->l2;
        s->triggerRight = p->r2;
        s->analogA = p->cross;
        s->analogB = p->square;
        s->err = PAD_ERR_NONE;
        connected |= PAD_CHAN0_BIT >> chan;
    }
    {
        // All on, all on with an RSX wait per draw, then each change alone, then none.
        static const unsigned modes[] = { 0, 8, 6, 5, 3, 7 };
        static unsigned held, mode;

        // Select is no GameCube button: each press moves the backend to the next A/B mode.
        if ((pads[0].buttons & PS3_PAD_SELECT) && !held) {
            unsigned m;

            mode = (mode + 1) % 6;
            m = modes[mode];
            ps3_diag.gx_ab = m;
            OSReport("gx A/B: mode %u: batching %s, VP slots %s, constants %s, RSX wait per draw %s\n", mode,
                     m & 1 ? "off" : "on", m & 2 ? "off" : "on", m & 4 ? "all" : "changed", m & 8 ? "on" : "off");
        }
        held = pads[0].buttons & PS3_PAD_SELECT;
    }
#ifdef PS3_PAD_AUTOPRESS
    // For RPCS3 runs without a pad: channel 0 holds A for 10 retraces from retrace PS3_PAD_AUTOPRESS.
    if (!pads[0].connected) {
        memset(&status[0], 0, sizeof(status[0]));
        if (VIGetRetraceCount() - PS3_PAD_AUTOPRESS < 10) {
            status[0].button = PAD_BUTTON_A;
            ps3_diag.trace = 1;
        }
        connected |= PAD_CHAN0_BIT;
    }
#endif
    return connected;
}

int PADReset(unsigned long mask)
{
    return TRUE;
}

BOOL PADRecalibrate(u32 mask)
{
    return TRUE;
}

void PADSetSpec(u32 spec) {}

// Reads happen when the game asks, so there is no sampling rate to set.
void PADSetSamplingRate(unsigned long msec) {}

void PADControlMotor(s32 chan, u32 command)
{
    ps3_pad_rumble(chan, command == PAD_MOTOR_RUMBLE);
}
