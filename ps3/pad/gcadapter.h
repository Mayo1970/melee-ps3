// The Wii U GameCube adapter (057e:0337) through PSL1GHT's usbd. Standard C types only, so SDK sources can
// include it.
#ifndef PS3_PAD_GCADAPTER_H
#define PS3_PAD_GCADAPTER_H

#include <stdbool.h>

#define PS3_GCADAPTER_PORTS 4

enum {
    PS3_GC_A = 1 << 0,
    PS3_GC_B = 1 << 1,
    PS3_GC_X = 1 << 2,
    PS3_GC_Y = 1 << 3,
    PS3_GC_LEFT = 1 << 4,
    PS3_GC_RIGHT = 1 << 5,
    PS3_GC_DOWN = 1 << 6,
    PS3_GC_UP = 1 << 7,
    PS3_GC_START = 1 << 8,
    PS3_GC_Z = 1 << 9,
    PS3_GC_R = 1 << 10,
    PS3_GC_L = 1 << 11,
};

typedef struct Ps3GcPad {
    bool connected;
    unsigned buttons;
    // As the pad sends them: 0-255, the sticks growing right and up, no origin taken off.
    unsigned char sx, sy, cx, cy, l, r;
} Ps3GcPad;

void ps3_gcadapter_init(void);
// The adapter's four ports; none connected while no adapter is attached.
void ps3_gcadapter_read(Ps3GcPad pads[PS3_GCADAPTER_PORTS]);
// Needs the adapter's second (power) plug.
void ps3_gcadapter_rumble(int port, bool on);

#endif
