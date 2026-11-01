// The rollback's SPU program (rb_shared.h): compares, copies and hashes the state's chunks for rollback.c, while the
// main thread waits. Runs on an SPU, or on a thread in the host builds.
#include <string.h>

#include "rb_shared.h"
#include "rbspu_io.h"

#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

// Chunks per round of transfers, and chunk indices per output transfer.
#define BATCH 8
#define OUT_MAX 1024

static RbShared sh_in __attribute__((aligned(128)));
static RbShared sh_out __attribute__((aligned(128)));
static RbChunk desc[BATCH] __attribute__((aligned(128)));
static RbApply apply[BATCH] __attribute__((aligned(128)));
static uint8_t mem_buf[BATCH][RB_CHUNK] __attribute__((aligned(128)));
static uint8_t shadow_buf[BATCH][RB_CHUNK] __attribute__((aligned(128)));
static uint32_t out[OUT_MAX] __attribute__((aligned(128)));
static uint32_t nout, out_off;

#ifdef __SPU__
static int same(const void* a, const void* b, uint32_t n)
{
    const vec_uint4* x = a;
    const vec_uint4* y = b;
    vec_uint4 acc0 = spu_splats(0u), acc1 = acc0;
    uint32_t i, q = n / 16;

    for (i = 0; i + 1 < q; i += 2) {
        acc0 = spu_or(acc0, spu_xor(x[i], y[i]));
        acc1 = spu_or(acc1, spu_xor(x[i + 1], y[i + 1]));
    }
    for (; i < q; i++) {
        acc0 = spu_or(acc0, spu_xor(x[i], y[i]));
    }
    return spu_extract(spu_orx(spu_or(acc0, acc1)), 0) == 0;
}

// rb_hash with the four lanes in one vector.
static uint32_t hash(const void* p, uint32_t n)
{
    const vec_uint4* x = p;
    const vec_uint4 k = spu_splats(0x9E3779B9u);
    vec_uint4 h = spu_splats(0x811C9DC5u);
    uint32_t i, l1, l2, l3;

    for (i = 0; i < n / 16; i++) {
        h = spu_add(spu_rl(spu_xor(h, x[i]), 7), k);
    }
    l1 = spu_extract(h, 1);
    l2 = spu_extract(h, 2);
    l3 = spu_extract(h, 3);
    return spu_extract(h, 0) ^ ((l1 << 8) | (l1 >> 24)) ^ ((l2 << 16) | (l2 >> 16)) ^ ((l3 << 24) | (l3 >> 8));
}
#else
static int same(const void* a, const void* b, uint32_t n)
{
    return memcmp(a, b, n) == 0;
}

static uint32_t hash(const void* p, uint32_t n)
{
    return rb_hash(p, n);
}
#endif

static void emit(uint32_t v)
{
    out[nout++] = v;
    if (nout == OUT_MAX) {
        rbio_put(out, sh_in.out_ea + out_off, sizeof(out));
        rbio_wait();
        out_off += sizeof(out);
        nout = 0;
    }
}

// The out buffer has room for whole 16 bytes past the last entry.
static void flush(void)
{
    if (nout != 0) {
        rbio_put(out, sh_in.out_ea + out_off, (nout * 4 + 15) & ~15u);
        rbio_wait();
    }
}

// RB_SAVE: a changed chunk's shadow goes into the next undo slot and the chunk into the shadow. RB_RESTORE: a changed
// chunk gets its shadow back. RB_HASH: every chunk's hash. Each emits the chunk's index or hash.
static uint32_t scan(uint32_t op)
{
    uint32_t c = 0, count = 0, i;

    while (c < sh_in.nchunks) {
        uint32_t n = sh_in.nchunks - c < BATCH ? sh_in.nchunks - c : BATCH;

        rbio_get(desc, sh_in.chunks_ea + c * sizeof(RbChunk), n * sizeof(RbChunk));
        rbio_wait();
        for (i = 0; i < n; i++) {
            rbio_get(mem_buf[i], desc[i].mem_ea, desc[i].len);
            if (op != RB_HASH) {
                rbio_get(shadow_buf[i], desc[i].shadow_ea, desc[i].len);
            }
        }
        rbio_wait();
        for (i = 0; i < n; i++) {
            if (op == RB_HASH) {
                emit(hash(mem_buf[i], desc[i].len));
                continue;
            }
            if (same(mem_buf[i], shadow_buf[i], desc[i].len)) {
                continue;
            }
            if (op == RB_SAVE) {
                uint32_t slot = (sh_in.head + count) % sh_in.pool_slots;

                rbio_put(shadow_buf[i], sh_in.pool_ea + slot * RB_CHUNK, desc[i].len);
                rbio_put(mem_buf[i], desc[i].shadow_ea, desc[i].len);
            } else {
                rbio_put(shadow_buf[i], desc[i].mem_ea, desc[i].len);
            }
            emit(c + i);
            count++;
        }
        // The buffers take the next round only after these puts.
        rbio_wait();
        c += n;
    }
    return count;
}

// RB_RESTORE, after the scan: each slot into its chunk and the chunk's shadow.
static void put_back(void)
{
    uint32_t a = 0, i;

    while (a < sh_in.napply) {
        uint32_t n = sh_in.napply - a < BATCH ? sh_in.napply - a : BATCH;

        rbio_get(apply, sh_in.apply_ea + a * sizeof(RbApply), n * sizeof(RbApply));
        rbio_wait();
        for (i = 0; i < n; i++) {
            rbio_get(mem_buf[i], apply[i].src_ea, apply[i].len);
        }
        rbio_wait();
        for (i = 0; i < n; i++) {
            rbio_put(mem_buf[i], apply[i].mem_ea, apply[i].len);
            rbio_put(mem_buf[i], apply[i].shadow_ea, apply[i].len);
        }
        rbio_wait();
        a += n;
    }
}

void rb_spu_main(uint32_t shared_ea)
{
    uint32_t done = 0;

    for (;;) {
        uint32_t count = 0;

        rbio_get(&sh_in, shared_ea, 128);
        rbio_wait();
        if (sh_in.seq == done) {
            rbio_wait_us(2);
            continue;
        }
        nout = out_off = 0;
        if (sh_in.op == RB_SAVE || sh_in.op == RB_RESTORE || sh_in.op == RB_HASH) {
            count = scan(sh_in.op);
            flush();
        }
        if (sh_in.op == RB_RESTORE) {
            put_back();
        }
        done = sh_in.seq;
        sh_out.done = done;
        sh_out.count = count;
        // After every transfer above: each round waited for its own.
        rbio_put(&sh_out.done, shared_ea + 128, 128);
        rbio_wait();
    }
}
