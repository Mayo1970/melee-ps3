// The vertex program: GX vertices from the queue (vx_shared.h) into RSX vertices in the vertex ring, with the
// steps vtx.c runs on the PPE. Runs on an SPU, or on a thread in the host builds.
#include <string.h>

#include "vx_shared.h"
#include "vxspu_io.h"

// Vertices per pass: their windows, list and output fit the buffers below.
#define CHUNK 64
#define MAX_GATHER (CHUNK * 16)
#define OUT_BYTES (16u << 10)

static VxShared sh_in __attribute__((aligned(128)));
static VxShared sh_out __attribute__((aligned(128)));
static uint8_t win[2 * VX_MAX_ENTRY] __attribute__((aligned(128)));
static VxLayoutBlock layout;
static VxArraysBlock arrays;
static VxGather list[MAX_GATHER] __attribute__((aligned(16)));
static const uint8_t* where[MAX_GATHER];
static uint8_t windows[MAX_GATHER * 32] __attribute__((aligned(128)));
static uint8_t out[OUT_BYTES] __attribute__((aligned(128)));

// Vertices v0 to v0 + nv - 1 of e, from in (the entry's GX vertices).
static void run_chunk(const VxEntry* e, const uint8_t* in, uint32_t v0, uint32_t nv)
{
    uint32_t n = 0, at = 0, v, i, g = 0;

    for (v = 0; v < nv; v++) {
        const uint8_t* src = in + (v0 + v) * e->in_size;

        for (i = 0; i < layout.nsteps; i++) {
            const VtxStep* st = &layout.step[i];
            const uint8_t* p = src + st->src;
            uint32_t idx, ea, lo;

            if (st->array == 0xFF) {
                continue;
            }
            idx = st->idx16 ? vtx_be16(p) : p[0];
            ea = arrays.ea[st->array] + idx * arrays.stride[st->array] + st->elem_off;
            // Whole 16-byte lines: never past the line of the element's last byte, so never into another page.
            lo = ea & ~15u;
            list[n].size = ((ea + vtx_step_bytes(st) + 15) & ~15u) - lo;
            list[n].ea = lo;
            where[n] = windows + at + (ea - lo);
            at += list[n].size;
            n++;
        }
    }
    if (n != 0) {
        vxio_gather(windows, list, n);
    }
    // Zeroed, so the bytes between attributes are the same on every run.
    memset(out, 0, (nv * e->out_size + 15) & ~15u);
    for (v = 0; v < nv; v++) {
        const uint8_t* src = in + (v0 + v) * e->in_size;
        uint8_t* d = out + v * e->out_size;

        for (i = 0; i < layout.nsteps; i++) {
            const VtxStep* st = &layout.step[i];

            vtx_step_run(st, st->array == 0xFF ? src + st->src : where[g++], d, e->def);
        }
    }
    vxio_put(out, e->out_ea + v0 * e->out_size, (nv * e->out_size + 15) & ~15u);
}

// False when the entry's sizes do not add up.
static int run_entry(const VxEntry* e)
{
    const uint8_t* p = (const uint8_t*) (e + 1);
    uint32_t chunk, v0;

    if (e->flags & VX_NEW_LAYOUT) {
        memcpy(&layout, p, sizeof(layout));
        p += sizeof(layout);
        if (layout.nsteps > VX_STEPS) {
            layout.nsteps = 0;
            return 0;
        }
    }
    if (e->flags & VX_NEW_ARRAYS) {
        memcpy(&arrays, p, sizeof(arrays));
        p += sizeof(arrays);
    }
    if (e->out_size == 0 || (e->out_size & 3) != 0 || (e->out_ea & 15) != 0 ||
        p + e->count * e->in_size > (const uint8_t*) e + e->size) {
        return 0;
    }
    // A multiple of 4 vertices of a multiple of 4 bytes: each chunk's output starts 16-byte aligned.
    chunk = (OUT_BYTES / e->out_size < CHUNK ? OUT_BYTES / e->out_size : CHUNK) & ~3u;
    if (chunk == 0) {
        return 0;
    }
    for (v0 = 0; v0 < e->count; v0 += chunk) {
        run_chunk(e, p, v0, e->count - v0 < chunk ? e->count - v0 : chunk);
    }
    return 1;
}

void vx_spu_main(uint32_t shared_ea)
{
    uint32_t tail = 0, done = 0, errors = 0;

    vxio_get(&sh_in, shared_ea, 128);
    for (;;) {
        uint32_t head, off, n, used = 0;

        vxio_get(&sh_in, shared_ea, 128);
        head = sh_in.head;
        if (head == tail) {
            vxio_wait_us(2);
            continue;
        }
        off = tail % VX_RING_SIZE;
        n = head - tail;
        n = n < VX_RING_SIZE - off ? n : VX_RING_SIZE - off;
        n = n < sizeof(win) ? n : sizeof(win);
        vxio_get(win, sh_in.ring_ea + off, n);
        while (used + sizeof(VxEntry) <= n) {
            const VxEntry* e = (const VxEntry*) (win + used);

            if (e->flags & VX_WRAP) {
                // The last entry in the ring: its bytes need not be read.
                used += e->size;
                done++;
                break;
            }
            if (e->size < sizeof(VxEntry) || e->size % VX_ENTRY_ALIGN != 0 || e->size > VX_MAX_ENTRY) {
                // The queue is lost: everything queued so far is skipped.
                used = head - tail;
                errors++;
                break;
            }
            if (e->size > n - used) {
                break;
            }
            if (!run_entry(e)) {
                errors++;
            }
            used += e->size;
            done++;
        }
        tail += used;
        sh_out.tail = tail;
        sh_out.done = done;
        sh_out.errors = errors;
        // After the vertices: each transfer above is done before the next one starts.
        vxio_put(&sh_out.tail, shared_ea + 128, 128);
    }
}
