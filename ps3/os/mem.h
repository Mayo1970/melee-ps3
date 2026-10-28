#ifndef PS3_OS_MEM_H
#define PS3_OS_MEM_H

#include <stdbool.h>

#include <dolphin/types.h>

// GameCube physical addresses for the memory the GP reads. Under TARGET_PS3, GX packs texture, TLUT and copy
// addresses as 24-bit counts of 32-byte units (GX_ADDR_BITS), so every registered region maps into 512 MB.
#define PS3_PHYS_LIMIT (512u << 20)

// Maps [base, base + size) to the next free physical range. Returns false when the physical range is full.
bool ps3_mem_add_region(void* base, u32 size);
// The flipper's MMIO registers (CP, PE, PI, MEM) as plain memory, by GameCube physical address.
volatile u16* ps3_mmio16(u32 paddr);

// Any thread: [p, p + size) was written (a cache flush, a DVD read, an ARAM copy, a .dat parse). Each write gets
// a number, kept per 4 KB page; the GP's caches compare it with the stamp of the data they derived.
void ps3_mem_written(const void* p, u32 size);
// The GP thread: the stamp to keep with data read from memory now. Every write numbered up to it is complete.
u32 ps3_mem_stamp(void);
// True when a write after stamp touched a page of physical [paddr, paddr + size).
bool ps3_mem_written_since(u32 paddr, u32 size, u32 stamp);

#endif
