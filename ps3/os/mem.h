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

#endif
