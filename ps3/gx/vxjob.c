// Vertex conversion on an SPU. See vxjob.h. GP thread only.
#include "vxjob.h"

#include <malloc.h>
#include <stdint.h>
#include <string.h>

#include <dolphin/os.h>

#include "../os/diag.h"
#include "../os/prof.h"
#include "../os/sync.h"
#include "../os/udplog.h"

_Static_assert(VX_STEPS == VTX_MAX_STEPS, "the SPU's layout block holds every step");
_Static_assert(VX_ARRAYS == VTX_ARRAYS, "the SPU's arrays block holds every array");
_Static_assert(sizeof(VxEntry) == VX_ENTRY_ALIGN, "an entry header is one alignment unit");

// A/B (Select): the conversion stays on the PPE.
#define VX_AB_PPE 33554432u
// A wait that lasts longer means the SPU is gone: the conversion goes back to the PPE.
#define WAIT_LIMIT_US 500000u

static VxShared* sh;
static u8* ring;
static u32 head;
static bool running;
// What the SPU holds: the last layout and arrays sent.
static const VtxLayout* sent_layout;
static u32 sent_id[5];
static VxArraysBlock sent_arrays;
static bool sent_arrays_ok;

#ifdef GX_SPU_CHECK
// Check build (host): each queued draw is also converted on the PPE, and compared once the SPU wrote it.
#define CHECK_JOBS 4096
static struct {
    const u8* out;
    u8* ref;
    u32 bytes;
} checks[CHECK_JOBS];
static u32 nchecks, check_jobs, check_errors;

static void check_done(void)
{
    u32 i;

    for (i = 0; i < nchecks; i++) {
        if (memcmp(checks[i].out, checks[i].ref, checks[i].bytes) != 0 && check_errors++ < 20) {
            ps3_log("vx check: draw at %p differs (%u bytes)\n", (const void*) checks[i].out, checks[i].bytes);
        }
        free(checks[i].ref);
        if (++check_jobs % 100000 == 0) {
            ps3_log("vx check: %u draws, %u differ\n", check_jobs, check_errors);
        }
    }
    nchecks = 0;
}

static void check_add(const VtxLayout* l, const GpState* s, const u8* in, u32 count, const u8* out)
{
    u32 bytes = count * l->out_size;
    u8* ref;

    if (nchecks == CHECK_JOBS) {
        vx_drain();
    }
    ref = calloc(1, bytes + 16);
    if (ref == NULL || !vtx_convert(l, s, in, count, ref)) {
        free(ref);
        return;
    }
    checks[nchecks].out = out;
    checks[nchecks].ref = ref;
    checks[nchecks].bytes = bytes;
    nchecks++;
}
#endif

bool vx_init(void)
{
    if (vx_start == NULL || sh != NULL) {
        return running;
    }
    sh = memalign(128, sizeof(VxShared));
    ring = memalign(128, VX_RING_SIZE);
    if (sh == NULL || ring == NULL || ((uintptr_t) sh >> 32) != 0 || ((uintptr_t) ring >> 32) != 0) {
        ps3_log("vx: no queue below 4 GB, the vertex conversion stays on the PPE\n");
        return false;
    }
    memset(sh, 0, sizeof(*sh));
    sh->ring_ea = (u32) (uintptr_t) ring;
    running = vx_start(sh);
    ps3_log("vx: vertex conversion on %s\n", running ? "an SPU" : "the PPE (the SPU program did not start)");
    return running;
}

bool vx_running(void)
{
    return running;
}

// Waits until at most left bytes of the queue are unfinished. False when the SPU stopped.
static bool wait_queue(u32 left)
{
    static PsMutex m = PS_MUTEX_INIT;
    static PsCond c = PS_COND_INIT;
    u32 spins = 0, slept = 0;
    uint64_t t;

    if (head - __atomic_load_n(&sh->tail, __ATOMIC_ACQUIRE) <= left) {
        return true;
    }
    t = ps3_prof_now();
    while (head - __atomic_load_n(&sh->tail, __ATOMIC_ACQUIRE) > left) {
        if (++spins < 4000) {
            // Low, then medium SMT priority: the main thread gets the core's issue slots meanwhile.
            __asm__ volatile("or 1,1,1\n\tor 2,2,2");
            continue;
        }
        if (slept >= WAIT_LIMIT_US) {
            ps3_log("vx: the SPU left %u bytes of the queue for 0.5 s (done %u, errors %u); the vertex conversion "
                    "goes back to the PPE\n",
                    head - sh->tail, sh->done, sh->errors);
            running = false;
            return false;
        }
        ps_mutex_lock(&m);
        ps_cond_timedwait(&c, &m, 20);
        ps_mutex_unlock(&m);
        slept += 20;
    }
    ps3_prof.spu_waits++;
    ps3_prof.spu_wait_ticks += ps3_prof_now() - t;
    return true;
}

// The entry's stores before the new head.
static void publish(u32 to)
{
    __atomic_store_n(&sh->head, to, __ATOMIC_RELEASE);
    head = to;
}

void vx_drain(void)
{
    if (running) {
        wait_queue(0);
    }
#ifdef GX_SPU_CHECK
    check_done();
#endif
}

bool vx_submit(const VtxLayout* l, const GpState* s, const u8* in, u32 count, u8* out)
{
    VxArraysBlock arr;
    VxEntry* e;
    u8* p;
    u32 in_bytes = count * l->in_size, size, off, used = 0, i;
    bool new_layout, new_arrays;

    if (!running || (ps3_diag.gx_ab & VX_AB_PPE) || ((uintptr_t) out & 15) != 0 || ((uintptr_t) out >> 32) != 0) {
        return false;
    }
    for (i = 0; i < l->nsteps; i++) {
        if (l->step[i].array != 0xFF) {
            used |= 1u << l->step[i].array;
        }
    }
    memset(&arr, 0, sizeof(arr));
    for (i = 0; i < VX_ARRAYS; i++) {
        if ((used >> i) & 1) {
            void* a = OSPhysicalToCached(s->array_base[i]);

            // The PPE conversion drops a draw whose array maps to nothing.
            if (a == NULL || ((uintptr_t) a >> 32) != 0) {
                return false;
            }
            arr.ea[i] = (u32) (uintptr_t) a;
            arr.stride[i] = (u8) s->array_stride[i];
        }
    }
    // A layouts slot is reused for another layout: its VCD and VAT name it.
    new_layout = l != sent_layout || l->vcd_lo != sent_id[0] || l->vcd_hi != sent_id[1] || l->vat_a != sent_id[2] ||
                 l->vat_b != sent_id[3] || l->vat_c != sent_id[4];
    new_arrays = used != 0 && (!sent_arrays_ok || memcmp(&arr, &sent_arrays, sizeof(arr)) != 0);
    size = sizeof(VxEntry) + (new_layout ? sizeof(VxLayoutBlock) : 0) + (new_arrays ? sizeof(VxArraysBlock) : 0) +
           in_bytes;
    size = (size + VX_ENTRY_ALIGN - 1) & ~(VX_ENTRY_ALIGN - 1);
    if (size > VX_MAX_ENTRY) {
        ps3_prof.spu_ppe++;
        return false;
    }
    off = head % VX_RING_SIZE;
    if (off + size > VX_RING_SIZE) {
        // The rest of the ring goes unused: an entry never crosses its end.
        if (!wait_queue(off)) {
            return false;
        }
        e = (VxEntry*) (ring + off);
        e->size = VX_RING_SIZE - off;
        e->count = 0;
        e->flags = VX_WRAP;
        publish(head + e->size);
        off = 0;
    }
    if (!wait_queue(VX_RING_SIZE - size)) {
        return false;
    }
    e = (VxEntry*) (ring + off);
    e->size = size;
    e->count = count;
    e->out_ea = (u32) (uintptr_t) out;
    e->in_size = l->in_size;
    e->out_size = l->out_size;
    e->flags = (new_layout ? VX_NEW_LAYOUT : 0) | (new_arrays ? VX_NEW_ARRAYS : 0);
    vtx_default_midx(s, e->def);
    p = (u8*) (e + 1);
    if (new_layout) {
        VxLayoutBlock* b = (VxLayoutBlock*) p;

        b->nsteps = l->nsteps;
        memcpy(b->step, l->step, l->nsteps * sizeof(VtxStep));
        p += sizeof(*b);
        sent_layout = l;
        sent_id[0] = l->vcd_lo;
        sent_id[1] = l->vcd_hi;
        sent_id[2] = l->vat_a;
        sent_id[3] = l->vat_b;
        sent_id[4] = l->vat_c;
    }
    if (new_arrays) {
        memcpy(p, &arr, sizeof(arr));
        p += sizeof(arr);
        sent_arrays = arr;
        sent_arrays_ok = true;
    }
    memcpy(p, in, in_bytes);
    publish(head + size);
    ps3_prof.spu_jobs++;
#ifdef GX_SPU_CHECK
    check_add(l, s, in, count, out);
#endif
    return true;
}
