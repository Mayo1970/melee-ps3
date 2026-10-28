// DS3 pads through PSL1GHT's ioPad. Standard C types only, so SDK sources can include it.
#ifndef PS3_PAD_PAD_CELL_H
#define PS3_PAD_PAD_CELL_H

#include <stdbool.h>

enum {
    PS3_PAD_LEFT = 1 << 0,
    PS3_PAD_RIGHT = 1 << 1,
    PS3_PAD_UP = 1 << 2,
    PS3_PAD_DOWN = 1 << 3,
    PS3_PAD_CROSS = 1 << 4,
    PS3_PAD_CIRCLE = 1 << 5,
    PS3_PAD_SQUARE = 1 << 6,
    PS3_PAD_TRIANGLE = 1 << 7,
    PS3_PAD_L1 = 1 << 8,
    PS3_PAD_R1 = 1 << 9,
    PS3_PAD_L2 = 1 << 10,
    PS3_PAD_R2 = 1 << 11,
    PS3_PAD_START = 1 << 12,
    PS3_PAD_SELECT = 1 << 13,
};

typedef struct Ps3Pad {
    bool connected;
    unsigned buttons;
    // Sticks 0-255 with 128 at the centre, 0 at the left and the top.
    unsigned char lx, ly, rx, ry;
    // Pressures 0-255; the digital state when the pad sends none.
    unsigned char l2, r2, cross, square;
} Ps3Pad;

void ps3_pad_init(void);
// The first n connected pads of the seven ports, in port order. A pad that sends no new data keeps its state.
void ps3_pad_read(Ps3Pad* pads, int n);
// Rumble on the pad that the last read gave as pads[index].
void ps3_pad_rumble(int index, bool on);

#endif
