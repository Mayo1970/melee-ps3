// The PS3 menu (ps3_options.h): Select on a DS3 pad, or Up with Z held on a GameCube pad, opens it outside matches.
#ifndef PS3_PAD_MENU_H
#define PS3_PAD_MENU_H

#include <stdbool.h>

#include <dolphin/pad.h>

// PADInit: loads the options and gives the presents the menu's picture.
void ps3_menu_init(void);
// PADRead's last step; select: Select went down on a DS3 pad. While the menu is open, and until the buttons that
// closed it are up, the game gets the pads at rest.
void ps3_menu_pad(PADStatus status[PAD_MAX_CONTROLLERS], bool select);

#endif
