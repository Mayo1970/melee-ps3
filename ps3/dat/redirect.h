// Forced into the native .dat readers' sources (-include): their allocations come from the pool.
#include <stdlib.h>

#include "pool.h"

#define malloc ps3_dat_malloc
#define calloc ps3_dat_calloc
#define realloc ps3_dat_realloc
#define free ps3_dat_free
