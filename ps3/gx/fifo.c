// The GP ring between the write-gather pipe and the GP thread. See fifo.h.
#include "fifo.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include <gx_pipe.h>

#include "../os/diag.h"
#include "../os/sync.h"

// The producer publishes at least this often, so the GP works while the game still writes.
#define PUBLISH_CHUNK 0x4000
// A store of up to 8 bytes may run past the ring end; the next slow path moves it to the ring start.
#define SLACK 8
// Larger reads are assembled piecewise in the scratch buffer, so a block can exceed the free space.
#define MAX_DIRECT_READ(size) ((size) / 4)

u8* __GXPipeWp;
u8* __GXPipeLimit;

const u8* gp_fifo_rp;
const u8* gp_fifo_avail;

static u8* ring;
static u32 ring_size;
// Stream position of ring[0] in the producer's and in the consumer's lap.
static u64 prod_base;
static u64 cons_base;
static u64 published;
static u64 consumed;

static PsMutex lock = PS_MUTEX_INIT;
static PsCond data_cond = PS_COND_INIT;
static PsCond space_cond = PS_COND_INIT;
static PsCond idle_cond = PS_COND_INIT;
static int cons_waiting;
static int prod_waiting;
static int kicked;
static void (*idle_work)(void);

static u8* scratch;
static u32 scratch_size;

void gp_fifo_init(u32 size)
{
    if (ring != NULL) {
        return;
    }
    ring_size = size;
    // 128-byte aligned for the cache lines the pipe writes.
    ring = memalign(128, size + SLACK);
    if (ring == NULL) {
        OSReport("gp_fifo_init: no memory for 0x%x bytes\n", size);
        abort();
    }
    __GXPipeWp = ring;
    __GXPipeLimit = ring;
    gp_fifo_rp = ring;
    gp_fifo_avail = ring;
}

static u64 load(u64* p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static void wait_consumed(u64 pos)
{
    if (load(&consumed) >= pos) {
        return;
    }
    ps_mutex_lock(&lock);
    __atomic_store_n(&prod_waiting, 1, __ATOMIC_SEQ_CST);
    ps3_diag.ring_space_wait = 1;
    while (load(&consumed) < pos) {
        ps_cond_wait(&space_cond, &lock);
    }
    ps3_diag.ring_space_wait = 0;
    __atomic_store_n(&prod_waiting, 0, __ATOMIC_SEQ_CST);
    ps_mutex_unlock(&lock);
}

static void publish(u64 pos)
{
    __atomic_store_n(&published, pos, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&cons_waiting, __ATOMIC_SEQ_CST)) {
        ps_mutex_lock(&lock);
        ps_cond_signal(&data_cond);
        ps_mutex_unlock(&lock);
    }
}

// Moves a store that ran past the ring end to the ring start, then publishes what is written.
static u64 wrap_and_publish(void)
{
    u64 pos;

    if (__GXPipeWp >= ring + ring_size) {
        u32 over = (u32) (__GXPipeWp - (ring + ring_size));

        // The ring start still holds the previous lap until the GP has taken it.
        wait_consumed(prod_base + over);
        memcpy(ring, ring + ring_size, over);
        prod_base += ring_size;
        __GXPipeWp = ring + over;
    }
    pos = prod_base + (u64) (__GXPipeWp - ring);
    publish(pos);
    return pos;
}

void __GXPipeMakeRoom(void)
{
    u64 pos;

    if (ring == NULL) {
        OSReport("GX pipe used before gp_fifo_init\n");
        abort();
    }
    pos = wrap_and_publish();
    for (;;) {
        // A store at pos must not reach bytes the GP has not taken.
        u64 limit = load(&consumed) + ring_size - SLACK;

        if (limit > prod_base + ring_size) {
            limit = prod_base + ring_size;
        }
        if (limit > pos + PUBLISH_CHUNK) {
            limit = pos + PUBLISH_CHUNK;
        }
        if (limit > pos) {
            __GXPipeLimit = ring + (limit - prod_base);
            return;
        }
        wait_consumed(load(&consumed) + 1);
    }
}

void __GXPipeFlush(void)
{
    if (ring != NULL) {
        wrap_and_publish();
    }
}

// The consumer's stream position.
static u64 cons_pos(void)
{
    return cons_base + (u64) (gp_fifo_rp - ring);
}

void gp_fifo_release(void)
{
    __atomic_store_n(&consumed, cons_pos(), __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&prod_waiting, __ATOMIC_SEQ_CST)) {
        ps_mutex_lock(&lock);
        ps_cond_signal(&space_cond);
        ps_mutex_unlock(&lock);
    }
}

// Waits until the stream holds more than pos; returns the published position.
static u64 wait_published(u64 pos)
{
    u64 pub = load(&published);

    if (pub > pos) {
        return pub;
    }
    ps_mutex_lock(&lock);
    __atomic_store_n(&cons_waiting, 1, __ATOMIC_SEQ_CST);
    ps3_diag.gp_wait = 1;
    while ((pub = load(&published)) <= pos) {
        // Not idle while it runs, for gp_fifo_sync.
        if (kicked) {
            kicked = 0;
            __atomic_store_n(&cons_waiting, 0, __ATOMIC_SEQ_CST);
            ps_mutex_unlock(&lock);
            idle_work();
            ps_mutex_lock(&lock);
            __atomic_store_n(&cons_waiting, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        ps_cond_broadcast(&idle_cond);
        ps_cond_wait(&data_cond, &lock);
    }
    ps3_diag.gp_wait = 0;
    __atomic_store_n(&cons_waiting, 0, __ATOMIC_SEQ_CST);
    ps_mutex_unlock(&lock);
    return pub;
}

static void wrap_reader(void)
{
    if (gp_fifo_rp == ring + ring_size) {
        gp_fifo_rp = ring;
        cons_base += ring_size;
    }
}

static void update_avail(u64 pub)
{
    u64 end = pub - cons_base;

    gp_fifo_avail = ring + (end < ring_size ? end : ring_size);
}

const u8* gp_fifo_take_slow(u32 n)
{
    const u8* p;
    u32 done;

    // The bytes taken so far are parsed; giving them back lets the producer go on while this waits.
    gp_fifo_release();
    wrap_reader();
    if (n <= MAX_DIRECT_READ(ring_size) && n <= (u32) (ring + ring_size - gp_fifo_rp)) {
        u64 pos = cons_pos();
        u64 pub = load(&published);

        while (pub < pos + n) {
            pub = wait_published(pub);
        }
        update_avail(pub);
        p = gp_fifo_rp;
        gp_fifo_rp += n;
        return p;
    }
    if (scratch_size < n) {
        free(scratch);
        scratch_size = n < 0x10000 ? 0x10000 : n;
        scratch = malloc(scratch_size);
        if (scratch == NULL) {
            OSReport("gp_fifo_take_slow: no memory for 0x%x bytes\n", scratch_size);
            abort();
        }
    }
    for (done = 0; done < n;) {
        u64 pos, pub;
        u32 chunk;

        wrap_reader();
        pos = cons_pos();
        pub = wait_published(pos);
        chunk = n - done;
        if (chunk > pub - pos) {
            chunk = (u32) (pub - pos);
        }
        if (chunk > (u32) (ring + ring_size - gp_fifo_rp)) {
            chunk = (u32) (ring + ring_size - gp_fifo_rp);
        }
        memcpy(scratch + done, gp_fifo_rp, chunk);
        gp_fifo_rp += chunk;
        done += chunk;
        gp_fifo_release();
    }
    wrap_reader();
    update_avail(load(&published));
    return scratch;
}

void gp_fifo_set_idle_work(void (*fn)(void))
{
    idle_work = fn;
}

void gp_fifo_kick(void)
{
    ps_mutex_lock(&lock);
    kicked = idle_work != NULL;
    ps_cond_signal(&data_cond);
    ps_mutex_unlock(&lock);
}

void gp_fifo_sync(void)
{
    u64 pos;

    __GXPipeFlush();
    pos = load(&published);
    ps_mutex_lock(&lock);
    while (!(__atomic_load_n(&cons_waiting, __ATOMIC_SEQ_CST) && load(&consumed) >= pos)) {
        ps_cond_wait(&idle_cond, &lock);
    }
    ps_mutex_unlock(&lock);
}
