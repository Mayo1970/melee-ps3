// The queue between the GP thread and the vertex program on an SPU (vxjob.c, vxspu.c). Both are big-endian;
// stdint types only. Every address is below 4 GB, as all user addresses on the PS3.
#ifndef PS3_GX_VX_SHARED_H
#define PS3_GX_VX_SHARED_H

#include <stdint.h>

#include "vtx_step.h"

// The entry ring; positions count bytes over its laps. An entry never crosses the ring end.
#define VX_RING_SIZE (256u << 10)
#define VX_ENTRY_ALIGN 32
// The largest entry; the SPU reads the ring in windows of twice this.
#define VX_MAX_ENTRY (16u << 10)
#define VX_STEPS 40
#define VX_ARRAYS 12

// Entry flags.
#define VX_WRAP 1       // no draw: the rest of the ring is unused, the next entry is at its start
#define VX_NEW_LAYOUT 2 // a VxLayoutBlock follows the header
#define VX_NEW_ARRAYS 4 // a VxArraysBlock follows (after the layout block)

// Then the blocks of the flags, then the count GX vertices of in_size bytes; 16-byte aligned each.
typedef struct VxEntry {
    uint32_t size;   // bytes of the entry, a multiple of VX_ENTRY_ALIGN
    uint32_t count;  // vertices
    uint32_t out_ea; // where the RSX vertices go, 16-byte aligned; the SPU writes up to the next 16 bytes
    uint16_t in_size, out_size;
    uint8_t flags;
    uint8_t def[9]; // the default matrix indices
    uint8_t pad[6];
} VxEntry;

// The steps of the vertex layout, kept by the SPU until the next one.
typedef struct VxLayoutBlock {
    uint32_t nsteps;
    uint32_t pad[3];
    VtxStep step[VX_STEPS];
} __attribute__((aligned(16))) VxLayoutBlock;

// The attribute arrays' addresses and strides, kept by the SPU until the next block.
typedef struct VxArraysBlock {
    uint32_t ea[VX_ARRAYS];
    uint8_t stride[VX_ARRAYS];
    uint8_t pad[4];
} __attribute__((aligned(16))) VxArraysBlock;

// The PPU writes the first 128 bytes, the SPU the second, so neither DMA overwrites the other's line.
typedef struct VxShared {
    uint32_t ring_ea;
    uint32_t head; // bytes queued
    uint32_t ppu_pad[30];

    uint32_t tail;   // bytes the SPU has finished
    uint32_t done;   // entries it has finished
    uint32_t errors; // entries it skipped: bad sizes
    uint32_t spu_pad[29];
} __attribute__((aligned(128))) VxShared;

#endif
