#ifndef PS3_STDBOOL_H
#define PS3_STDBOOL_H

// The game's bool is int (MSL), and the native .dat readers use the same layout.
typedef int bool;
#define true 1
#define false 0

#endif
