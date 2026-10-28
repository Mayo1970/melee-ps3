// The .dat pool: one heap in a region with physical addresses. See pool.h.
#include "pool.h"

#include <malloc.h>
#include <string.h>

#include <dolphin/os.h>

#include "../os/heap.h"
#include "../os/mem.h"
#include "../os/sync.h"

// Each archive's data is copied once more, with its objects in native layout.
#define POOL_SIZE (48u << 20)

static PsHeap heap;
static PsMutex lock = PS_MUTEX_INIT;

bool ps3_dat_pool_init(void)
{
    void* base = memalign(32, POOL_SIZE);

    if (base == NULL || !ps3_mem_add_region(base, POOL_SIZE)) {
        return false;
    }
    ps_heap_init(&heap, base, (char*) base + POOL_SIZE);
    return true;
}

void* ps3_dat_malloc(size_t size)
{
    void* p;

    ps_mutex_lock(&lock);
    p = ps_heap_alloc(&heap, size);
    ps_mutex_unlock(&lock);
    if (p == NULL) {
        OSReport("ps3_dat_malloc: no room for %u bytes\n", (u32) size);
    }
    return p;
}

void* ps3_dat_calloc(size_t n, size_t size)
{
    void* p;

    if (size != 0 && n > (size_t) -1 / size) {
        return NULL;
    }
    p = ps3_dat_malloc(n * size);
    if (p != NULL) {
        memset(p, 0, n * size);
    }
    return p;
}

void* ps3_dat_realloc(void* ptr, size_t size)
{
    size_t old;
    void* p;

    if (ptr == NULL) {
        return ps3_dat_malloc(size);
    }
    old = ps_heap_block_size(ptr);
    if (size <= old) {
        return ptr;
    }
    p = ps3_dat_malloc(size);
    if (p != NULL) {
        memcpy(p, ptr, old);
        ps3_dat_free(ptr);
    }
    return p;
}

void ps3_dat_free(void* ptr)
{
    if (ptr != NULL) {
        ps_mutex_lock(&lock);
        ps_heap_free(&heap, ptr);
        ps_mutex_unlock(&lock);
    }
}
