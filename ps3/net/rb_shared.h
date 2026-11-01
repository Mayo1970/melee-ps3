// The jobs between the rollback (rollback.c) and its SPU program (rbspu.c). Both are big-endian; stdint types only.
// Every address is below 4 GB, as all user addresses on the PS3.
#ifndef PS3_NET_RB_SHARED_H
#define PS3_NET_RB_SHARED_H

#include <stdint.h>

#define RB_CHUNK 4096u

enum { RB_SAVE = 1, RB_RESTORE = 2, RB_HASH = 3 };

// A chunk of the state and its shadow: 128-byte aligned, len a multiple of 16 up to RB_CHUNK.
typedef struct RbChunk {
    uint32_t mem_ea, shadow_ea, len, pad;
} RbChunk;

// An undo log slot to put back into a chunk and its shadow.
typedef struct RbApply {
    uint32_t src_ea, mem_ea, shadow_ea, len;
} RbApply;

// The PPU writes the first 128 bytes, the SPU the second, so neither DMA overwrites the other's line.
typedef struct RbShared {
    uint32_t seq;        // a new job while it differs from done
    uint32_t op;
    uint32_t chunks_ea;  // RbChunk[nchunks]
    uint32_t nchunks;    // the chunks to scan, from the first
    uint32_t pool_ea;    // RB_SAVE: the undo log, pool_slots slots of RB_CHUNK bytes
    uint32_t pool_slots;
    uint32_t head;       // RB_SAVE: the slot count to fill from (over laps)
    uint32_t out_ea;     // RB_SAVE, RB_RESTORE: the chunks that differed; RB_HASH: a hash per chunk
    uint32_t apply_ea;   // RB_RESTORE: RbApply[napply], put back after the scan
    uint32_t napply;
    uint32_t ppu_pad[22];

    uint32_t done;  // the last job finished
    uint32_t count; // RB_SAVE, RB_RESTORE: the chunks that differed
    uint32_t spu_pad[30];
} __attribute__((aligned(128))) RbShared;

// Four lanes over the 32-bit words; the SPU program computes the same with vector ops.
static inline uint32_t rb_hash(const uint8_t* p, uint32_t n)
{
    uint32_t lane[4] = { 0x811C9DC5u, 0x811C9DC5u, 0x811C9DC5u, 0x811C9DC5u };
    uint32_t i, k;

    for (i = 0; i + 16 <= n; i += 16) {
        for (k = 0; k < 4; k++) {
            uint32_t w = (uint32_t) p[i + 4 * k] << 24 | (uint32_t) p[i + 4 * k + 1] << 16 |
                         (uint32_t) p[i + 4 * k + 2] << 8 | p[i + 4 * k + 3];
            uint32_t x = lane[k] ^ w;

            lane[k] = ((x << 7) | (x >> 25)) + 0x9E3779B9u;
        }
    }
    return lane[0] ^ ((lane[1] << 8) | (lane[1] >> 24)) ^ ((lane[2] << 16) | (lane[2] >> 16)) ^
           ((lane[3] << 24) | (lane[3] >> 8));
}

#endif
