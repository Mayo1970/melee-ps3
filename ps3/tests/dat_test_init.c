// The .dat pool for the readers' host test, whose main is upstream's (tools/dat-cli/native/tests/hsd.c).
#include <stdlib.h>

#include "../dat/pool.h"

__attribute__((constructor)) static void init_pool(void)
{
    if (!ps3_dat_pool_init()) {
        abort();
    }
}
