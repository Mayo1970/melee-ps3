#ifndef PS3_DVD_H
#define PS3_DVD_H

#include <stdbool.h>

// Disc for the SDK's DVD functions: a .iso/.gcm/.ciso image, or an extracted disc folder that holds
// sys/boot.bin, sys/fst.bin and files/.
bool ps3_dvd_mount(const char* path);

// Searches PS3_APP_DIR/games and /dev_usbNNN/games, subfolders too. PS3_APP_DIR/disc.txt can name the disc
// by game ID or path; without it the first GALE01 revision 2 disc is used.
bool ps3_dvd_mount_default(void);

// Runs the callbacks of finished reads. The OS layer calls it where the GameCube took the DVD interrupt,
// with interrupts disabled. A callback in a read call breaks devcom, which marks itself busy after it.
void ps3_dvd_dispatch(void);

// Called after each finished read, so the OS layer can wake the thread that dispatches.
void ps3_dvd_set_notify(void (*notify)(void));

#endif
