// The SDK's OSAlloc heaps. Its 32-byte cell header has five fields, 40 bytes with LP64; this one keeps
// three, so blocks stay 32-byte aligned behind a 32-byte header. See heap.h.
#include "heap.h"

#include <stdint.h>

#include <dolphin/os.h>

#define ALIGNMENT 32
#define HEADER 32
// A remainder smaller than this stays with the allocated block.
#define MIN_CELL (HEADER + ALIGNMENT)

struct PsHeapCell {
    PsHeapCell* prev;
    PsHeapCell* next;
    // Bytes, the header included.
    long size;
};

_Static_assert(sizeof(PsHeapCell) <= HEADER, "the cell header fits in front of a 32-byte aligned block");

static uintptr_t round_up(uintptr_t x)
{
    return (x + ALIGNMENT - 1) & ~(uintptr_t) (ALIGNMENT - 1);
}

static uintptr_t round_down(uintptr_t x)
{
    return x & ~(uintptr_t) (ALIGNMENT - 1);
}

static PsHeapCell* add_front(PsHeapCell* list, PsHeapCell* cell)
{
    cell->next = list;
    cell->prev = NULL;
    if (list != NULL) {
        list->prev = cell;
    }
    return cell;
}

static PsHeapCell* extract(PsHeapCell* list, PsHeapCell* cell)
{
    if (cell->next != NULL) {
        cell->next->prev = cell->prev;
    }
    if (cell->prev == NULL) {
        return cell->next;
    }
    cell->prev->next = cell->next;
    return list;
}

// Inserts cell in address order and merges it with the free neighbours it touches.
static PsHeapCell* insert(PsHeapCell* list, PsHeapCell* cell)
{
    PsHeapCell* prev = NULL;
    PsHeapCell* next = list;

    while (next != NULL && next < cell) {
        prev = next;
        next = next->next;
    }
    cell->prev = prev;
    cell->next = next;
    if (next != NULL) {
        next->prev = cell;
        if ((char*) cell + cell->size == (char*) next) {
            cell->size += next->size;
            cell->next = next->next;
            if (cell->next != NULL) {
                cell->next->prev = cell;
            }
        }
    }
    if (prev == NULL) {
        return cell;
    }
    prev->next = cell;
    if ((char*) prev + prev->size == (char*) cell) {
        prev->size += cell->size;
        prev->next = cell->next;
        if (prev->next != NULL) {
            prev->next->prev = prev;
        }
    }
    return list;
}

void ps_heap_init(PsHeap* heap, void* start, void* end)
{
    uintptr_t s = round_up((uintptr_t) start);
    uintptr_t e = round_down((uintptr_t) end);
    PsHeapCell* cell = (PsHeapCell*) s;

    heap->free = NULL;
    heap->allocated = NULL;
    heap->start = heap->top = s;
    if (e < s + MIN_CELL) {
        heap->size = 0;
        return;
    }
    cell->prev = NULL;
    cell->next = NULL;
    cell->size = (long) (e - s);
    heap->size = cell->size;
    heap->free = cell;
    heap->top = s + HEADER;
}

void* ps_heap_alloc(PsHeap* heap, size_t size)
{
    long need = (long) round_up(size + HEADER);
    PsHeapCell* cell;
    uintptr_t wrote;

    for (cell = heap->free; cell != NULL; cell = cell->next) {
        if (cell->size >= need) {
            break;
        }
    }
    if (cell == NULL) {
        return NULL;
    }
    if (cell->size - need < MIN_CELL) {
        heap->free = extract(heap->free, cell);
        wrote = (uintptr_t) cell + (uintptr_t) cell->size;
    } else {
        PsHeapCell* rest = (PsHeapCell*) ((char*) cell + need);

        rest->size = cell->size - need;
        rest->prev = cell->prev;
        rest->next = cell->next;
        if (rest->next != NULL) {
            rest->next->prev = rest;
        }
        if (rest->prev != NULL) {
            rest->prev->next = rest;
        } else {
            heap->free = rest;
        }
        cell->size = need;
        wrote = (uintptr_t) rest + HEADER;
    }
    if (wrote > heap->top) {
        heap->top = wrote;
    }
    heap->allocated = add_front(heap->allocated, cell);
    return (char*) cell + HEADER;
}

void ps_heap_free(PsHeap* heap, void* ptr)
{
    PsHeapCell* cell = (PsHeapCell*) ((char*) ptr - HEADER);

    heap->allocated = extract(heap->allocated, cell);
    heap->free = insert(heap->free, cell);
}

size_t ps_heap_block_size(void* ptr)
{
    return (size_t) ((PsHeapCell*) ((char*) ptr - HEADER))->size - HEADER;
}

static long list_size(PsHeapCell* list, int ordered)
{
    PsHeapCell* prev = NULL;
    PsHeapCell* cell;
    long total = 0;

    for (cell = list; cell != NULL; prev = cell, cell = cell->next) {
        if (cell->prev != prev || cell->size < MIN_CELL - ALIGNMENT || (ordered && prev != NULL && prev >= cell)) {
            return -1;
        }
        total += cell->size;
    }
    return total;
}

long ps_heap_check(PsHeap* heap)
{
    long free_bytes = list_size(heap->free, 1);
    long used_bytes = list_size(heap->allocated, 0);

    if (free_bytes < 0 || used_bytes < 0 || free_bytes + used_bytes != heap->size) {
        return -1;
    }
    return free_bytes;
}

// The SDK's interface over an array of heaps that OSInitAlloc takes from the arena.
volatile OSHeapHandle __OSCurrHeap = -1;

static PsHeap* heaps;
static int heap_count;

PsHeap* ps3_heap_table(int* count)
{
    *count = heap_count;
    return heaps;
}

static PsHeap* get(int heap)
{
    return heap >= 0 && heap < heap_count && heaps[heap].size >= 0 ? &heaps[heap] : NULL;
}

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
    int i;

    heaps = (PsHeap*) round_up((uintptr_t) arenaStart);
    heap_count = maxHeaps;
    for (i = 0; i < maxHeaps; i++) {
        heaps[i].size = -1;
        heaps[i].free = NULL;
        heaps[i].allocated = NULL;
        heaps[i].start = heaps[i].top = 0;
    }
    __OSCurrHeap = -1;
    return (void*) round_up((uintptr_t) (heaps + maxHeaps));
}

int OSCreateHeap(void* start, void* end)
{
    int i;

    for (i = 0; i < heap_count; i++) {
        if (heaps[i].size < 0) {
            ps_heap_init(&heaps[i], start, end);
            return i;
        }
    }
    OSReport("OSCreateHeap: all %d heaps are in use\n", heap_count);
    return -1;
}

void OSDestroyHeap(int heap)
{
    if (get(heap) != NULL) {
        heaps[heap].size = -1;
    }
}

int OSSetCurrentHeap(int heap)
{
    int old = __OSCurrHeap;

    __OSCurrHeap = heap;
    return old;
}

void* OSAllocFromHeap(int heap, unsigned long size)
{
    PsHeap* h = get(heap);

    return h != NULL ? ps_heap_alloc(h, size) : NULL;
}

void OSFreeToHeap(int heap, void* ptr)
{
    PsHeap* h = get(heap);

    if (h != NULL && ptr != NULL) {
        ps_heap_free(h, ptr);
    }
}

long OSCheckHeap(int heap)
{
    PsHeap* h = get(heap);
    long free_bytes = h != NULL ? ps_heap_check(h) : -1;

    if (free_bytes < 0) {
        OSReport("OSCheckHeap: heap %d is broken\n", heap);
    }
    return free_bytes;
}
