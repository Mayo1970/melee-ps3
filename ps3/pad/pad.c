// The SDK's PAD calls over DS3 pads and the Wii U GameCube adapter: Cross, Square, Circle, Triangle are A, B, X, Y;
// R1 Z, L2/R2 analog L/R, right stick C. Adapter port N is channel N; DS3 pads take the free channels in order.
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include "../os/diag.h"
#include "gcadapter.h"
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

static const struct {
    unsigned adapter;
    u16 gc;
} gc_buttons[] = {
    { PS3_GC_LEFT, PAD_BUTTON_LEFT }, { PS3_GC_RIGHT, PAD_BUTTON_RIGHT }, { PS3_GC_DOWN, PAD_BUTTON_DOWN },
    { PS3_GC_UP, PAD_BUTTON_UP },     { PS3_GC_Z, PAD_TRIGGER_Z },        { PS3_GC_R, PAD_TRIGGER_R },
    { PS3_GC_L, PAD_TRIGGER_L },      { PS3_GC_A, PAD_BUTTON_A },         { PS3_GC_B, PAD_BUTTON_B },
    { PS3_GC_X, PAD_BUTTON_X },       { PS3_GC_Y, PAD_BUTTON_Y },         { PS3_GC_START, PAD_BUTTON_START },
};

// A GameCube pad's rest position, taken when it appears, as the SI origin command does.
static struct {
    bool taken;
    unsigned char sx, sy, cx, cy, l, r;
} origin[PAD_MAX_CONTROLLERS];

// The DS3 pad index each channel got from the last read, -1 for none or an adapter pad.
static int chan_ds3[PAD_MAX_CONTROLLERS] = { -1, -1, -1, -1 };

static s8 axis(unsigned char v, bool flip)
{
    int a = flip ? 128 - (int) v : (int) v - 128;

    return (s8) (a > 127 ? 127 : a);
}

static s8 gc_axis(unsigned char v, unsigned char rest)
{
    int a = (int) v - (int) rest;

    return (s8) (a > 127 ? 127 : a < -128 ? -128 : a);
}

static u8 gc_trigger(unsigned char v, unsigned char rest)
{
    return v > rest ? (u8) (v - rest) : 0;
}

static void read_ds3(PADStatus* s, const Ps3Pad* p)
{
    size_t i;

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
}

static void read_gc(PADStatus* s, const Ps3GcPad* p, int chan)
{
    size_t i;

    if (!origin[chan].taken) {
        origin[chan].taken = true;
        origin[chan].sx = p->sx;
        origin[chan].sy = p->sy;
        origin[chan].cx = p->cx;
        origin[chan].cy = p->cy;
        origin[chan].l = p->l;
        origin[chan].r = p->r;
    }
    for (i = 0; i < sizeof(gc_buttons) / sizeof(gc_buttons[0]); i++) {
        if (p->buttons & gc_buttons[i].adapter) {
            s->button |= gc_buttons[i].gc;
        }
    }
    s->stickX = gc_axis(p->sx, origin[chan].sx);
    s->stickY = gc_axis(p->sy, origin[chan].sy);
    s->substickX = gc_axis(p->cx, origin[chan].cx);
    s->substickY = gc_axis(p->cy, origin[chan].cy);
    s->triggerLeft = gc_trigger(p->l, origin[chan].l);
    s->triggerRight = gc_trigger(p->r, origin[chan].r);
}

BOOL PADInit(void)
{
    static bool done;

    if (!done) {
        done = true;
        ps3_pad_init();
        ps3_gcadapter_init();
    }
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    Ps3Pad pads[PAD_MAX_CONTROLLERS];
    Ps3GcPad gc[PS3_GCADAPTER_PORTS];
    u32 connected = 0;
    int chan, k;

    ps3_diag.pad_reads++;
    ps3_pad_read(pads, PAD_MAX_CONTROLLERS);
    ps3_gcadapter_read(gc);
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        chan_ds3[chan] = -1;
        if (!gc[chan].connected) {
            origin[chan].taken = false;
        }
    }
    // DS3 pad k keeps channel k unless an adapter pad has it; then it takes the first free channel.
    for (k = 0; k < PAD_MAX_CONTROLLERS; k++) {
        if (pads[k].connected && !gc[k].connected) {
            chan_ds3[k] = k;
        }
    }
    for (k = 0; k < PAD_MAX_CONTROLLERS; k++) {
        if (!pads[k].connected || chan_ds3[k] == k) {
            continue;
        }
        for (chan = 0; chan < PAD_MAX_CONTROLLERS && (gc[chan].connected || chan_ds3[chan] >= 0); chan++) {
        }
        if (chan < PAD_MAX_CONTROLLERS) {
            chan_ds3[chan] = k;
        }
    }
    for (chan = 0; chan < PAD_MAX_CONTROLLERS; chan++) {
        PADStatus* s = &status[chan];

        memset(s, 0, sizeof(*s));
        if (gc[chan].connected) {
            read_gc(s, &gc[chan], chan);
        } else if (chan_ds3[chan] >= 0) {
            read_ds3(s, &pads[chan_ds3[chan]]);
        } else {
            s->err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        s->err = PAD_ERR_NONE;
        connected |= PAD_CHAN0_BIT >> chan;
    }
    {
        // As built; vertex conversion on the PPE; the old bindings after a present (the black flicker); the old depth
        // output; the flicker detector; then the black flicker's earlier tests (runs 46-52) and specular lights off.
        static const unsigned modes[] = { 0,       33554432, 16777216, 4194304, 2097152, 1048576, 524288, 262144,
                                          131072,  65536,    16,       4096,    8192,    16384,   32768,  2048 };
        static const char* const names[] = { "as built",
                                             "vertex conversion on the PPE (no SPU jobs)",
                                             "the old texture bindings after a present (the black flicker)",
                                             "the old depth output (z + w)",
                                             "flicker detector: one-frame setup changes go to the log",
                                             "later draws win depth ties (depth offset)",
                                             "the old alpha test (two-compare tests off)",
                                             "fighter shadows sample white",
                                             "shadow copies read back (slow)",
                                             "no texture unit holds an EFB copy's destination",
                                             "an RSX barrier after each draw",
                                             "an RSX barrier after cached list draws",
                                             "an RSX barrier after ring draws",
                                             "an RSX barrier before fragment program changes",
                                             "an RSX barrier before vertex format changes",
                                             "specular lights off" };
        static unsigned held, mode;

        // Select is no GameCube button: each press moves the backend to the next A/B mode.
        if ((pads[0].buttons & PS3_PAD_SELECT) && !held) {
            mode = (mode + 1) % (sizeof(modes) / sizeof(modes[0]));
            ps3_diag.gx_ab = modes[mode];
            OSReport("gx A/B: mode %u: %s\n", mode, names[mode]);
        }
        held = pads[0].buttons & PS3_PAD_SELECT;
    }
    {
        static unsigned held;

        // L1 is no GameCube button either: a mark in the log, to time what the player saw.
        if ((pads[0].buttons & PS3_PAD_L1) && !held) {
            OSReport("pad: mark\n");
        }
        held = pads[0].buttons & PS3_PAD_L1;
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
    if (chan < 0 || chan >= PAD_MAX_CONTROLLERS) {
        return;
    }
    if (chan_ds3[chan] >= 0) {
        ps3_pad_rumble(chan_ds3[chan], command == PAD_MOTOR_RUMBLE);
    } else {
        ps3_gcadapter_rumble(chan, command == PAD_MOTOR_RUMBLE);
    }
}
