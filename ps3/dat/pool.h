// The memory of the native .dat readers. GX reads the textures, display lists and vertex arrays they keep, so
// it has GameCube physical addresses (ps3/os/mem.c). redirect.h sends their malloc and free here.
#ifndef PS3_DAT_POOL_H
#define PS3_DAT_POOL_H

#include <stdbool.h>
#include <stddef.h>

bool ps3_dat_pool_init(void);
void* ps3_dat_malloc(size_t size);
void* ps3_dat_calloc(size_t n, size_t size);
void* ps3_dat_realloc(void* ptr, size_t size);
void ps3_dat_free(void* ptr);
// The bytes allocated now and at most, for the logs.
void ps3_dat_pool_usage(size_t* now, size_t* most);
// The pool's memory, for the rollback's survey.
void ps3_dat_pool_range(void** base, size_t* size);
// While on, frees wait: a rollback may restore game state that still points at the block. Off frees them.
void ps3_dat_pool_defer(bool on);

#endif
