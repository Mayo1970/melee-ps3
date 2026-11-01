// The .dat pool: one heap in a region with physical addresses. See pool.h.
#include "pool.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include "../os/heap.h"
#include "../os/mem.h"
#include "../os/sync.h"

// The native objects of the live archives; their data stays in the game's buffers. 48 MB ran out in run 24.
#define POOL_SIZE (64u << 20)

static PsHeap heap;
static PsMutex lock = PS_MUTEX_INIT;
static size_t used, peak;
static void* pool_base;
// The frees that wait for ps3_dat_pool_defer(false), each block once.
static bool defer;
static void** deferred;
static size_t ndeferred, deferred_cap;

bool ps3_dat_pool_init(void)
{
    void* base = memalign(32, POOL_SIZE);

    if (base == NULL || !ps3_mem_add_region(base, POOL_SIZE)) {
        return false;
    }
    ps_heap_init(&heap, base, (char*) base + POOL_SIZE);
    pool_base = base;
    return true;
}

void ps3_dat_pool_range(void** base, size_t* size)
{
    *base = pool_base;
    *size = POOL_SIZE;
}

// The caller holds lock. A block that does not fit in the list stays allocated.
static void keep(void* ptr)
{
    size_t i;

    for (i = 0; i < ndeferred; i++) {
        if (deferred[i] == ptr) {
            return;
        }
    }
    if (ndeferred == deferred_cap) {
        size_t cap = deferred_cap != 0 ? deferred_cap * 2 : 256;
        void** grown = realloc(deferred, cap * sizeof(*grown));

        if (grown == NULL) {
            return;
        }
        deferred = grown;
        deferred_cap = cap;
    }
    deferred[ndeferred++] = ptr;
}

void ps3_dat_pool_defer(bool on)
{
    size_t i, n;

    ps_mutex_lock(&lock);
    defer = on;
    n = on ? 0 : ndeferred;
    for (i = 0; i < n; i++) {
        used -= ps_heap_block_size(deferred[i]);
        ps_heap_free(&heap, deferred[i]);
    }
    if (!on) {
        ndeferred = 0;
    }
    ps_mutex_unlock(&lock);
    if (n != 0) {
        OSReport("dat: %u deferred frees done\n", (u32) n);
    }
}

void* ps3_dat_malloc(size_t size)
{
    void* p;

    ps_mutex_lock(&lock);
    p = ps_heap_alloc(&heap, size);
    if (p != NULL) {
        used += ps_heap_block_size(p);
        peak = used > peak ? used : peak;
    }
    ps_mutex_unlock(&lock);
    if (p == NULL) {
        OSReport("ps3_dat_malloc: no room for %u bytes\n", (u32) size);
    } else {
        // The reader fills the block next; the GP's caches of what was there before go.
        ps3_mem_written(p, (u32) size);
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
        if (defer) {
            keep(ptr);
        } else {
            used -= ps_heap_block_size(ptr);
            ps_heap_free(&heap, ptr);
        }
        ps_mutex_unlock(&lock);
    }
}

void ps3_dat_pool_usage(size_t* now, size_t* most)
{
    ps_mutex_lock(&lock);
    *now = used;
    *most = peak;
    ps_mutex_unlock(&lock);
}
