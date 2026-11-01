// A heap as the SDK's OSAlloc keeps one: first fit over an address-ordered free list that merges neighbours,
// blocks 32-byte aligned with a 32-byte header. The OS heaps (OSCreateHeap) and the .dat pool use it.
#ifndef PS3_OS_HEAP_H
#define PS3_OS_HEAP_H

#include <stddef.h>
#include <stdint.h>

typedef struct PsHeapCell PsHeapCell;

typedef struct PsHeap {
    // -1: not in use.
    long size;
    PsHeapCell* free;
    PsHeapCell* allocated;
    // The first cell, and the end of what the heap ever wrote (blocks and cell headers): the rollback saves
    // [start, top), and memory above top stays as it was.
    uintptr_t start, top;
} PsHeap;

void ps_heap_init(PsHeap* heap, void* start, void* end);
// NULL when no free block is large enough.
void* ps_heap_alloc(PsHeap* heap, size_t size);
void ps_heap_free(PsHeap* heap, void* ptr);
// The usable size of an allocated block.
size_t ps_heap_block_size(void* ptr);
// The free bytes, or -1 when the lists are broken.
long ps_heap_check(PsHeap* heap);
// The OS heap table that OSInitAlloc put in the arena, and its length.
PsHeap* ps3_heap_table(int* count);
// The whole arena that OSInit allocated (os.c), heaps and the rest.
void ps3_os_arena(unsigned char** base, unsigned int* size);

#endif
