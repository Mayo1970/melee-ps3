// The PS3 menu's options (ps3/pad/menu.c), under TARGET_PS3. Select opens the menu, outside matches only.
#ifndef PS3_OPTIONS_H
#define PS3_OPTIONS_H

#include <stdbool.h>

typedef struct Ps3Options {
    bool widescreen;      // the picture at 16:9; the render only, the logic camera stays at 4:3
    bool tap_jump[4];     // per port: up on the control stick jumps
    bool cstick_tilts[4]; // per port: the C-stick gives tilts instead of smash attacks
} Ps3Options;

// They change only between matches, so both consoles of a netplay match can agree on them at its start.
extern Ps3Options ps3_options;

// fn_8016E730 has set up a match (on), the scene loop ended (off). The menu closes at a match start.
void ps3_menu_match(bool on);

#endif
