// GameCube addresses over the PS3 address space, and the flipper's MMIO registers as plain memory.
#include "mem.h"

#include <stdbool.h>
#include <stdint.h>

#include <dolphin/os.h>

#define MAX_REGIONS 8
#define MMIO_BASE 0x0C000000
#define MMIO_SIZE 0x9000
// The flipper's uncached register window, which the SDK uses as constants (GXFIFO_ADDR). RSX local memory
// is mapped near it, so the range stays this narrow.
#define GC_UNCACHED_BASE 0xC0000000u
#define GC_REGS_START 0xCC000000u
#define GC_REGS_END 0xCC010000u
// Physical 0 stays unmapped, so a null pointer never translates to a valid address.
#define PHYS_START 0x8000

typedef struct Region {
    uintptr_t base;
    u32 size;
    u32 phys;
} Region;

static Region regions[MAX_REGIONS];
static int nregions;
static u32 phys_next = PHYS_START;
static u16 mmio[MMIO_SIZE / 2] __attribute__((aligned(32)));
static bool reported_unmapped;

bool ps3_mem_add_region(void* base, u32 size)
{
    // 32-byte aligned on both sides, so GX's 32-byte units keep their meaning.
    uintptr_t start = (uintptr_t) base & ~(uintptr_t) 31;
    u64 len = (u64) ((uintptr_t) base - start) + size;
    Region* r;

    if (nregions == MAX_REGIONS || (u64) phys_next + len > PS3_PHYS_LIMIT) {
        OSReport("ps3_mem_add_region: no room for %p + 0x%x\n", base, size);
        return false;
    }
    r = &regions[nregions];
    r->base = start;
    r->size = (u32) len;
    r->phys = phys_next;
    phys_next = (u32) ((phys_next + len + 0xFFF) & ~0xFFFull);
    __atomic_store_n(&nregions, nregions + 1, __ATOMIC_RELEASE);
    return true;
}

volatile u16* ps3_mmio16(u32 paddr)
{
    return &mmio[(paddr - MMIO_BASE) / 2];
}

#define PAGE_SHIFT 12

// The number of the last write to each physical page. A write raises its pages, then counts itself done.
static u32 page_write[PS3_PHYS_LIMIT >> PAGE_SHIFT];
static u32 writes_started, writes_done;
static u32 safe_stamp;

void ps3_mem_written(const void* p, u32 size)
{
    uintptr_t a = (uintptr_t) p;
    int n = __atomic_load_n(&nregions, __ATOMIC_ACQUIRE);
    u32 g, page, last;
    int i;

    for (i = 0; i < n; i++) {
        if (a - regions[i].base < regions[i].size) {
            break;
        }
    }
    if (i == n || size == 0) {
        return;
    }
    if (size > regions[i].size - (u32) (a - regions[i].base)) {
        size = regions[i].size - (u32) (a - regions[i].base);
    }
    g = __atomic_add_fetch(&writes_started, 1, __ATOMIC_ACQ_REL);
    page = (regions[i].phys + (u32) (a - regions[i].base)) >> PAGE_SHIFT;
    last = (regions[i].phys + (u32) (a - regions[i].base) + size - 1) >> PAGE_SHIFT;
    for (; page <= last; page++) {
        u32 old = __atomic_load_n(&page_write[page], __ATOMIC_RELAXED);

        // Two threads may write one page: the larger number stays.
        while ((s32) (g - old) > 0 &&
               !__atomic_compare_exchange_n(&page_write[page], &old, g, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        }
    }
    __atomic_add_fetch(&writes_done, 1, __ATOMIC_RELEASE);
}

u32 ps3_mem_stamp(void)
{
    u32 done = __atomic_load_n(&writes_done, __ATOMIC_ACQUIRE);

    // done read first: equal counts mean no write was in flight then, so every page holds its number.
    if (__atomic_load_n(&writes_started, __ATOMIC_ACQUIRE) == done) {
        safe_stamp = done;
    }
    return safe_stamp;
}

bool ps3_mem_written_since(u32 paddr, u32 size, u32 stamp)
{
    u32 page = paddr >> PAGE_SHIFT, last;

    if (size == 0 || paddr >= PS3_PHYS_LIMIT) {
        return false;
    }
    last = (paddr + size - 1) >> PAGE_SHIFT;
    last = last >= (PS3_PHYS_LIMIT >> PAGE_SHIFT) ? (PS3_PHYS_LIMIT >> PAGE_SHIFT) - 1 : last;
    for (; page <= last; page++) {
        if ((s32) (__atomic_load_n(&page_write[page], __ATOMIC_RELAXED) - stamp) > 0) {
            return true;
        }
    }
    return false;
}

// Regions are added in OSInit, before the threads that translate start (thread creation orders those writes), so
// the translations read nregions with a plain load: an acquire load costs an isync on the PPE.
u32 OSCachedToPhysical(void* caddr)
{
    uintptr_t p = (uintptr_t) caddr;
    int n = __atomic_load_n(&nregions, __ATOMIC_RELAXED);
    int i;

    for (i = 0; i < n; i++) {
        if (p - regions[i].base < regions[i].size) {
            return regions[i].phys + (u32) (p - regions[i].base);
        }
    }
    if (!reported_unmapped) {
        reported_unmapped = true;
        OSReport("OSCachedToPhysical: %p is in no mapped region\n", caddr);
    }
    return 0;
}

void* OSPhysicalToCached(u32 paddr)
{
    int n = __atomic_load_n(&nregions, __ATOMIC_RELAXED);
    int i;

    for (i = 0; i < n; i++) {
        if (paddr - regions[i].phys < regions[i].size) {
            return (void*) (regions[i].base + (paddr - regions[i].phys));
        }
    }
    return NULL;
}

void* OSPhysicalToUncached(u32 paddr)
{
    if (paddr - MMIO_BASE < MMIO_SIZE) {
        return (u8*) mmio + (paddr - MMIO_BASE);
    }
    return OSPhysicalToCached(paddr);
}

u32 OSUncachedToPhysical(void* ucaddr)
{
    uintptr_t p = (uintptr_t) ucaddr;

    if (p - (uintptr_t) mmio < MMIO_SIZE) {
        return MMIO_BASE + (u32) (p - (uintptr_t) mmio);
    }
    if (p >= GC_REGS_START && p < GC_REGS_END) {
        return (u32) (p - GC_UNCACHED_BASE);
    }
    return OSCachedToPhysical(ucaddr);
}

void* OSCachedToUncached(void* caddr)
{
    return caddr;
}

void* OSUncachedToCached(void* ucaddr)
{
    return ucaddr;
}
