// Saved game states and logic frames run again (ps3_rollback.h): the statics, the heap table and the HSD heap. The
// store is a shadow of the latest state plus, per older frame, the old contents of the 4 KB chunks that changed.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/ft/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/axdriver.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/state.h>

#include <ps3_netplay.h>
#include <ps3_rollback.h>

#include "../dat/pool.h"
#include "../os/alarm.h"
#include "../os/heap.h"
#include "../os/mem.h"
#include "../os/prof.h"
#include "../os/sys.h"
#include "rb_shared.h"

extern char __rb_data_start[], __rb_data_end[], __rb_bss_start[], __rb_bss_end[];

// The SPU program's start (rbjob_cell.c, tests/rbspu_host.c).
bool rb_start(RbShared* shared) __attribute__((weak));

#define CHUNK 4096u
// Frames whose pad channels and gameplay values are kept, more than the deepest re-run.
#define INPUTS 64
#define MAX_DEPTH 8
#define NRANGES 5
// The HSD heap is the last range: the scan stops at its top.
#define R_HEAP (NRANGES - 1)
#define MAX_KEPT 64
// Frame starts kept in the undo log, and the most frames it holds.
#define RING 64
#define MAX_SPAN 16
// The heap may grow this much during the match; a match whose heap grows more ends the session (host: 3 MB at most).
#define HEAP_ROOM (8u << 20)
// The undo log's room, in chunks.
#define POOL_CHUNKS 2048
// Differing chunks logged per frame, and frames logged per session.
#define DIFF_CHUNKS 24
#define DIFF_FRAMES 16
// The survey hashes every page at these session frames and logs what changed between them.
#define SURVEY_FIRST 120
#define SURVEY_STEP 60
#define SURVEY_LAST 480
// Sounds kept per frame for its re-runs.
#define FRAME_SOUNDS 16

enum { MODE_OFF, MODE_SYNC, MODE_SURVEY };

typedef struct Range {
    u8* p;
    size_t n;
} Range;

static const char* const range_names[NRANGES] = { "data", "bss", "heap table", "buffer", "heap" };

typedef struct Input {
    PADStatus stat[4];
    bool have;
} Input;

typedef struct Stats {
    u32 saves, restores, reruns, differ, play_differ, chunks, chunks_max, applied;
    uint64_t save_ticks, restore_ticks, rerun_ticks, hash_ticks;
} Stats;

// What a match's outcome rests on, per fighter: a frame that runs again must leave the same. The render between
// two frames changes allocators and caches, so only a frame's first re-run can match all memory.
typedef struct FighterView {
    u32 present, motion, ground;
    float facing, vel[3], pos[3], anim, percent, hitlag, shield;
} FighterView;

typedef struct Gameplay {
    u32 seed;
    s32 frame;
    s32 stocks[6];
    FighterView f[12];
} Gameplay;

typedef struct Kept {
    OSAlarm* at;
    OSAlarm bytes;
} Kept;

// The voices a frame's last run started: its re-runs take them again instead of starting new ones.
typedef struct FrameSounds {
    s32 frame;
    int n;
    struct {
        s32 id, vid;
        bool claimed;
    } s[FRAME_SOUNDS];
} FrameSounds;

static int mode = -1;
// The session plays a netplay match (ps3_netplay.h).
static bool net;
// Sync test: the frames run again after each checked frame, and every how many frames a check comes.
static int depth = 1, every = 1;
static bool armed, active, rerunning, ended_log, in_frame;
static s32 frame, rerun_frame;
static Input inputs[INPUTS];
static Gameplay plays[INPUTS];
static FrameSounds sounds[INPUTS];
static u32 play_frames, diff_frames;
static Stats st, last;
static PsHeap* hsd_heap;
static uintptr_t heap_end;

// The store, in one block from the high end of the HSD heap: the game does not use that memory in a match.
static u8* block;
static Range ranges[NRANGES];
static u32 range_first[NRANGES + 1];
static Range cover;
static u32 nchunks;
// Each chunk's memory and its own CHUNK-sized, 128-byte aligned slot in the shadow.
static RbChunk* chunks;
static u8* shadow;
static size_t shadow_size;
static u8* pool;
static u32* log_chunk;
// The SPU job's output, a restore's list of slots to put back, and the restore count per chunk that dedups it.
static u32* out_buf;
static RbApply* apply_buf;
static u32* stamp;
static u32 epoch;
static RbShared rb_sh __attribute__((aligned(128)));
// -1 not tried, 0 the PPE does the work, 1 the SPU program.
static int spu_state = -1;
// Entries [tail, head) live; frame k's undo is [frame_start[k], frame_start[k + 1]) for oldest <= k < shadow_frame.
static u32 head, tail;
static u32 frame_start[RING];
static s32 oldest, shadow_frame;
// Sync test: the chunk hashes after the last run of a frame from the state the store holds for it, and a scratch row.
static u32* ref_hashes;
static u32 ref_count[MAX_DEPTH];
static s32 ref_frame[MAX_DEPTH];
static u32* now_hashes;

// What a restore leaves as it is: the queued alarms, and the shape blend buffers that the GP may still read.
static Kept kept[MAX_KEPT];
static int nkept;
static void* shape_at[2];
static u32 shape_size[2];
static u8 shape_bytes[2][HSD_DEFAULT_MAX_SHAPE_VERTICES * 12];

static void read_mode(void)
{
    const char* s = NULL;

#ifdef __linux__
    s = getenv("MELEE_HOST_RB");
#elif defined(PS3_RB_SYNC)
#ifndef PS3_RB_EVERY
#define PS3_RB_EVERY 1
#endif
    static char buf[32];

    snprintf(buf, sizeof(buf), "sync:%d:%d", PS3_RB_SYNC, PS3_RB_EVERY);
    s = buf;
#elif defined(PS3_RB_SURVEY)
    s = "survey";
#endif
    mode = MODE_OFF;
    if (s != NULL && strncmp(s, "sync", 4) == 0) {
        mode = MODE_SYNC;
        sscanf(s + 4, ":%d:%d", &depth, &every);
        depth = depth < 1 ? 1 : depth > MAX_DEPTH ? MAX_DEPTH : depth;
        every = every < 1 ? 1 : every;
    } else if (s != NULL && strcmp(s, "survey") == 0) {
        mode = MODE_SURVEY;
    }
}

static u32 us(uint64_t ticks)
{
    u32 mhz = (u32) (ps3_sys_timebase_freq() / 1000000);

    return mhz != 0 ? (u32) (ticks / mhz) : 0;
}

static void keep_alarm(OSAlarm* alarm, void* arg)
{
    if (nkept < MAX_KEPT) {
        kept[nkept].at = alarm;
        kept[nkept].bytes = *alarm;
        nkept++;
    }
}

static void keep(void)
{
    int i;

    nkept = 0;
    ps3_alarm_foreach(keep_alarm, NULL);
    HSD_PObjShapeBuffers(shape_at, shape_size);
    for (i = 0; i < 2; i++) {
        if (shape_at[i] != NULL && shape_size[i] <= sizeof(shape_bytes[i])) {
            memcpy(shape_bytes[i], shape_at[i], shape_size[i]);
        } else {
            shape_at[i] = NULL;
        }
    }
}

// A shape buffer goes back only when the restored state has it too: else the heap holds a free block there.
static void put_back(void)
{
    void* at[2];
    u32 size[2];
    int i;

    for (i = 0; i < nkept; i++) {
        *kept[i].at = kept[i].bytes;
    }
    HSD_PObjShapeBuffers(at, size);
    for (i = 0; i < 2; i++) {
        if (shape_at[i] != NULL && at[i] == shape_at[i] && size[i] == shape_size[i]) {
            memcpy(shape_at[i], shape_bytes[i], shape_size[i]);
        }
    }
}

// A frame's first run: no voices yet.
static void sounds_first(s32 f)
{
    sounds[f % INPUTS].frame = f;
    sounds[f % INPUTS].n = 0;
}

static void sounds_rerun(s32 f)
{
    FrameSounds* fs = &sounds[f % INPUTS];
    int i;

    if (fs->frame != f) {
        sounds_first(f);
    }
    for (i = 0; i < fs->n; i++) {
        fs->s[i].claimed = false;
    }
}

// The voices the re-run did not take again belong to inputs that turned out wrong: they stop.
static void sounds_rerun_end(s32 f)
{
    FrameSounds* fs = &sounds[f % INPUTS];
    int i, n = 0;

    for (i = 0; i < fs->n; i++) {
        if (fs->s[i].claimed) {
            fs->s[n++] = fs->s[i];
        } else {
            HSD_AudioSFXKeyOff(fs->s[i].vid);
        }
    }
    fs->n = n;
}

#define MEM(c) ((u8*) (uintptr_t) chunks[c].mem_ea)
#define SHADOW(c) ((u8*) (uintptr_t) chunks[c].shadow_ea)
#define LEN(c) (chunks[c].len)

// The chunks to scan: all statics and the heap table, and the heap up to its top (above it the heap holds zeros, as
// the session cleared it, until the top moves past).
static u32 scan_end(void)
{
    uintptr_t used = hsd_heap->top - hsd_heap->start;

    return range_first[R_HEAP] + (u32) ((used + CHUNK - 1) / CHUNK);
}

static void evict(void)
{
    oldest++;
    tail = frame_start[oldest % RING];
}

// Runs one job on the SPU program while this thread waits at low SMT priority, so the GP thread gets the core.
static u32 run_job(u32 op, u32 n, u32 napply)
{
    u32 seq = rb_sh.seq + 1;
    u64 spins = 0;

    rb_sh.op = op;
    rb_sh.chunks_ea = (u32) (uintptr_t) chunks;
    rb_sh.nchunks = n;
    rb_sh.pool_ea = (u32) (uintptr_t) pool;
    rb_sh.pool_slots = POOL_CHUNKS;
    rb_sh.head = head;
    rb_sh.out_ea = (u32) (uintptr_t) out_buf;
    rb_sh.apply_ea = (u32) (uintptr_t) apply_buf;
    rb_sh.napply = napply;
    __atomic_store_n(&rb_sh.seq, seq, __ATOMIC_RELEASE);
    while (__atomic_load_n(&rb_sh.done, __ATOMIC_ACQUIRE) != seq) {
        __asm__ volatile("or 1,1,1\n\tor 2,2,2");
        if (++spins == 200000000ull) {
            OSReport("rollback: the SPU program has not finished job %u (op %u)\n", seq, op);
        }
    }
    return rb_sh.count;
}

// Keeps chunk c's shadow, the state of frame shadow_frame, in the undo log (the PPE's way).
static bool push(u32 c)
{
    u32 slot;

    if (head - tail == POOL_CHUNKS) {
        if (oldest >= shadow_frame) {
            OSReport("rollback: one frame changed more than the undo log's %u chunks\n", POOL_CHUNKS);
            return false;
        }
        evict();
    }
    slot = head % POOL_CHUNKS;
    memcpy(pool + (size_t) slot * CHUNK, SHADOW(c), LEN(c));
    log_chunk[slot] = c;
    head++;
    return true;
}

// The SPU's save filled slots from head on, over the oldest when the log was full: those frames go.
static bool save_spu(u32 end, u32* changed)
{
    u32 i;

    *changed = run_job(RB_SAVE, end, 0);
    if (*changed > POOL_CHUNKS) {
        OSReport("rollback: one frame changed %u chunks, more than the undo log's %u\n", *changed, POOL_CHUNKS);
        return false;
    }
    for (i = 0; i < *changed; i++) {
        log_chunk[(head + i) % POOL_CHUNKS] = out_buf[i];
    }
    head += *changed;
    while (head - tail > POOL_CHUNKS) {
        if (oldest >= shadow_frame) {
            OSReport("rollback: one frame changed more than the undo log's %u chunks\n", POOL_CHUNKS);
            return false;
        }
        evict();
    }
    return true;
}

// Before frame f runs. The shadow holds frame f - 1's state, or f's after a restore to it. Netplay skips the saves
// of frames whose inputs are known: then only f's state stays, as no older frame runs again.
static bool save(s32 f)
{
    u32 c, end, changed = 0;
    bool gap = f > shadow_frame + 1;

    if (hsd_heap->top > (uintptr_t) (ranges[R_HEAP].p + ranges[R_HEAP].n)) {
        OSReport("rollback: the HSD heap grew %u KB, more than the store's %u KB\n",
                 (u32) ((hsd_heap->top - hsd_heap->start) >> 10), (u32) (ranges[R_HEAP].n >> 10));
        return false;
    }
    if (f == shadow_frame) {
        st.saves++;
        return true;
    }
    if (f < shadow_frame || (gap && !net)) {
        OSReport("rollback: frame %d saved after frame %d\n", f, shadow_frame);
        return false;
    }
    if (gap) {
        oldest = shadow_frame;
        tail = head;
    }
    end = scan_end();
    if (spu_state == 1) {
        if (!save_spu(end, &changed)) {
            return false;
        }
    } else {
        for (c = 0; c < end; c++) {
            if (memcmp(MEM(c), SHADOW(c), LEN(c)) != 0) {
                if (!push(c)) {
                    return false;
                }
                memcpy(SHADOW(c), MEM(c), LEN(c));
                changed++;
            }
        }
    }
    shadow_frame = f;
    frame_start[f % RING] = head;
    if (gap) {
        oldest = f;
        tail = head;
    }
    while (shadow_frame - oldest > MAX_SPAN) {
        evict();
    }
    st.saves++;
    st.chunks += changed;
    st.chunks_max = changed > st.chunks_max ? changed : st.chunks_max;
    return true;
}

// Each chunk the frames f to shadow_frame - 1 changed, once, with its oldest saved contents.
static u32 apply_list(s32 f)
{
    u32 n = 0;
    s32 k;

    epoch++;
    for (k = f; k < shadow_frame; k++) {
        u32 b = frame_start[k % RING], e = frame_start[(k + 1) % RING];

        for (; b != e; b++) {
            u32 slot = b % POOL_CHUNKS, c = log_chunk[slot];

            if (stamp[c] != epoch) {
                stamp[c] = epoch;
                apply_buf[n].src_ea = (u32) (uintptr_t) (pool + (size_t) slot * CHUNK);
                apply_buf[n].mem_ea = chunks[c].mem_ea;
                apply_buf[n].shadow_ea = chunks[c].shadow_ea;
                apply_buf[n].len = chunks[c].len;
                n++;
            }
        }
    }
    return n;
}

// The state before frame f: memory back to the shadow, then each changed chunk's oldest saved contents. The GP's
// caches hear of each chunk written.
static bool restore(s32 f)
{
    BOOL enabled;
    u32 c, end, napply, i;

    if (f < oldest || f > shadow_frame) {
        OSReport("rollback: frame %d is not in the store (%d to %d)\n", f, oldest, shadow_frame);
        return false;
    }
    enabled = OSDisableInterrupts();
    keep();
    // The top before the table goes back: chunks the frames since the save took lie below it.
    end = scan_end();
    napply = apply_list(f);
    if (spu_state == 1) {
        u32 n = run_job(RB_RESTORE, end, napply);

        for (i = 0; i < n; i++) {
            ps3_mem_written(MEM(out_buf[i]), LEN(out_buf[i]));
        }
        st.applied += n;
    } else {
        for (c = 0; c < end; c++) {
            if (memcmp(MEM(c), SHADOW(c), LEN(c)) != 0) {
                memcpy(MEM(c), SHADOW(c), LEN(c));
                ps3_mem_written(MEM(c), LEN(c));
                st.applied++;
            }
        }
        for (i = 0; i < napply; i++) {
            const void* src = (const void*) (uintptr_t) apply_buf[i].src_ea;

            memcpy((void*) (uintptr_t) apply_buf[i].mem_ea, src, apply_buf[i].len);
            memcpy((void*) (uintptr_t) apply_buf[i].shadow_ea, src, apply_buf[i].len);
        }
    }
    for (i = 0; i < napply; i++) {
        ps3_mem_written((void*) (uintptr_t) apply_buf[i].mem_ea, apply_buf[i].len);
    }
    st.applied += napply;
    head = frame_start[f % RING];
    shadow_frame = f;
    put_back();
    OSRestoreInterrupts(enabled);
    // The restored render caches name GX state of an older frame.
    HSD_StateInvalidate(-1);
    st.restores++;
    return true;
}

// Zeros the bytes of [p, p + n) that lie in [at, at + size).
static void mask(u8* copy, const u8* p, size_t n, const void* at, size_t size)
{
    const u8* a = at;
    const u8* lo = a > p ? a : p;
    const u8* hi = a + size < p + n ? a + size : p + n;

    if (lo < hi) {
        memset(copy + (lo - p), 0, (size_t) (hi - lo));
    }
}

// Each scanned chunk's rb_hash, the bytes keep() keeps aside: they move with the alarm thread or the render.
static u32 hash_state(u32* out)
{
    static u8 copy[CHUNK];
    u32 c, end = scan_end();
    int i;

    keep();
    if (spu_state == 1) {
        run_job(RB_HASH, end, 0);
        memcpy(out, out_buf, (size_t) end * sizeof(u32));
    }
    for (c = 0; c < end; c++) {
        const u8* p = MEM(c);
        size_t n = LEN(c);
        bool masked = false;

        for (i = 0; i < nkept && !masked; i++) {
            masked = (const u8*) kept[i].at < p + n && (const u8*) (kept[i].at + 1) > p;
        }
        for (i = 0; i < 2 && !masked; i++) {
            masked = shape_at[i] != NULL && (const u8*) shape_at[i] < p + n &&
                     (const u8*) shape_at[i] + shape_size[i] > p;
        }
        if (masked) {
            memcpy(copy, p, n);
            for (i = 0; i < nkept; i++) {
                mask(copy, p, n, kept[i].at, sizeof(OSAlarm));
            }
            for (i = 0; i < 2; i++) {
                if (shape_at[i] != NULL) {
                    mask(copy, p, n, shape_at[i], shape_size[i]);
                }
            }
            out[c] = rb_hash(copy, (u32) n);
        } else if (spu_state != 1) {
            out[c] = rb_hash(p, (u32) n);
        }
    }
    return end;
}

static int range_of(u32 c)
{
    int i = 0;

    while (i + 1 < NRANGES && c >= range_first[i + 1]) {
        i++;
    }
    return i;
}

// The first re-run frame against its first run's chunk hashes; logs the chunks that differ.
static void compare_state(s32 f, const u32* then, u32 then_n)
{
    u32 n = hash_state(now_hashes), c, differ = 0;
    u32 common = n < then_n ? n : then_n;

    for (c = 0; c < common; c++) {
        if (now_hashes[c] != then[c]) {
            int r = range_of(c);

            if (differ < DIFF_CHUNKS && diff_frames < DIFF_FRAMES) {
                OSReport("rollback: frame %d: %s +%#x (%p) differs from its first run\n", f, range_names[r],
                         (c - range_first[r]) * CHUNK, MEM(c));
            }
            differ++;
        }
    }
    if (n != then_n && diff_frames < DIFF_FRAMES) {
        OSReport("rollback: frame %d: the heap's top differs from its first run (%u and %u chunks)\n", f, n, then_n);
    }
    if (differ != 0 || n != then_n) {
        diff_frames++;
        st.differ++;
    }
}

static void view_gameplay(Gameplay* g)
{
    int slot, k;

    memset(g, 0, sizeof(*g));
    g->seed = *HSD_RandSeedPtr;
    g->frame = frame;
    for (slot = 0; slot < 6; slot++) {
        g->stocks[slot] = Player_GetStocks(slot);
        for (k = 0; k < 2; k++) {
            HSD_GObj* gobj = Player_GetEntityAtIndex(slot, k);
            FighterView* v = &g->f[slot * 2 + k];
            Fighter* fp;

            if (gobj == NULL || gobj->user_data == NULL) {
                continue;
            }
            fp = gobj->user_data;
            v->present = 1;
            v->motion = fp->motion_id;
            v->ground = fp->ground_or_air;
            v->facing = fp->facing_dir;
            v->vel[0] = fp->self_vel.x;
            v->vel[1] = fp->self_vel.y;
            v->vel[2] = fp->self_vel.z;
            v->pos[0] = fp->cur_pos.x;
            v->pos[1] = fp->cur_pos.y;
            v->pos[2] = fp->cur_pos.z;
            v->anim = fp->cur_anim_frame;
            v->percent = fp->dmg.x1830_percent;
            v->hitlag = fp->dmg.x195c_hitlag_frames;
            v->shield = fp->shield_health;
        }
    }
}

// Logs the first value that differs from the frame's first run; true when one does. nth: the frame's place in the
// re-run, 0 for the restored state's own frame.
static bool compare_gameplay(const Gameplay* then, s32 f, int nth)
{
    static const char* const names[] = { "present", "motion", "ground", "facing", "velocity x", "velocity y",
                                         "velocity z", "x", "y", "z", "animation frame", "percent", "hitlag",
                                         "shield" };
    Gameplay now;
    int i, k;

    view_gameplay(&now);
    now.frame = then->frame;
    if (memcmp(&now, then, sizeof(now)) == 0) {
        return false;
    }
    st.play_differ++;
    if (play_frames++ >= DIFF_FRAMES) {
        return true;
    }
    if (now.seed != then->seed) {
        OSReport("rollback: frame %d (re-run frame %d): RNG seed %08x, first run %08x\n", f, nth, now.seed, then->seed);
    }
    for (i = 0; i < 6; i++) {
        if (now.stocks[i] != then->stocks[i]) {
            OSReport("rollback: frame %d: player %d stocks %d, first run %d\n", f, i, now.stocks[i], then->stocks[i]);
        }
    }
    for (i = 0; i < 12; i++) {
        const u32* a = (const u32*) &now.f[i];
        const u32* b = (const u32*) &then->f[i];

        for (k = 0; k < (int) (sizeof(FighterView) / 4); k++) {
            if (a[k] != b[k]) {
                float fa, fb;

                memcpy(&fa, &a[k], 4);
                memcpy(&fb, &b[k], 4);
                OSReport("rollback: frame %d: fighter %d.%d %s %08x (%g), first run %08x (%g)\n", f, i / 2, i % 2,
                         names[k], a[k], (double) fa, b[k], (double) fb);
                break;
            }
        }
    }
    return true;
}

static void stop(void);

static void hash_ref(s32 j)
{
    uint64_t t = ps3_prof_now();

    ref_count[j % MAX_DEPTH] = hash_state(ref_hashes + (size_t) (j % MAX_DEPTH) * nchunks);
    ref_frame[j % MAX_DEPTH] = j;
    st.hash_ticks += ps3_prof_now() - t;
}

// Sync test: the last depth frames run again (saving again, as rollback does) and must give their first runs' gameplay
// values; the first must also match all memory of the last run from its saved state (depth > 1: the last check's).
static bool sync_frame(Ps3RbFrame run, void* arg)
{
    s32 f = frame, from, j;
    uint64_t t = ps3_prof_now();
    bool ended;

    if (!save(f)) {
        stop();
        return run(arg);
    }
    st.save_ticks += ps3_prof_now() - t;
    sounds_first(f);
    in_frame = true;
    ended = run(arg);
    in_frame = false;
    frame++;
    if (ended) {
        return true;
    }
    view_gameplay(&plays[f % INPUTS]);
    if (depth == 1) {
        hash_ref(f);
    }
    from = f - depth + 1;
    if (from < 0 || f % every != 0) {
        return false;
    }
    t = ps3_prof_now();
    if (!restore(from)) {
        stop();
        return false;
    }
    st.restore_ticks += ps3_prof_now() - t;
    rerunning = true;
    for (j = from; j <= f; j++) {
        t = ps3_prof_now();
        if (!save(j)) {
            rerunning = false;
            stop();
            return false;
        }
        st.save_ticks += ps3_prof_now() - t;
        rerun_frame = j;
        sounds_rerun(j);
        in_frame = true;
        t = ps3_prof_now();
        ended = run(arg);
        st.rerun_ticks += ps3_prof_now() - t;
        in_frame = false;
        sounds_rerun_end(j);
        st.reruns++;
        if (ended) {
            OSReport("rollback: frame %d ended the scene when it ran again\n", j);
            break;
        }
        if (j == from && ref_frame[j % MAX_DEPTH] == j) {
            t = ps3_prof_now();
            compare_state(j, ref_hashes + (size_t) (j % MAX_DEPTH) * nchunks, ref_count[j % MAX_DEPTH]);
            st.hash_ticks += ps3_prof_now() - t;
        }
        // The next check starts from this frame's re-saved state.
        if (depth > 1 && j == f - depth + 2) {
            hash_ref(j);
        }
        compare_gameplay(&plays[j % INPUTS], j, j - from);
    }
    rerunning = false;
    return ended;
}

// After each run of frame f: its gameplay values go to the netplay's desync check.
static void frame_done(s32 f)
{
    Gameplay g;

    view_gameplay(&g);
    g.frame = f;
    ps3_net_frame_done(f, rb_hash((const u8*) &g, sizeof(g)), &g, sizeof(g));
}

// The rollback cannot go on (no store, a frame no longer in it): the match goes on without the other console.
static void net_fail(void)
{
    OSReport("rollback: the netplay session cannot go on at frame %d; the pairing ends\n", frame);
    ps3_net_cancel();
    stop();
}

// Frames from..to - 1 run again from the state before from, with the inputs known now. 1 when one of them ends the
// scene (*end), -1 when the store fails.
static int net_rerun(s32 from, s32 to, s32 confirmed, Ps3RbFrame run, void* arg, s32* end)
{
    uint64_t t = ps3_prof_now();
    s32 j;
    bool ended;

    if (!restore(from)) {
        return -1;
    }
    st.restore_ticks += ps3_prof_now() - t;
    rerunning = true;
    for (j = from; j < to; j++) {
        // A frame whose remote input is known never runs again: its state need not be kept.
        t = ps3_prof_now();
        if (j > confirmed && !save(j)) {
            rerunning = false;
            return -1;
        }
        st.save_ticks += ps3_prof_now() - t;
        rerun_frame = j;
        sounds_rerun(j);
        in_frame = true;
        t = ps3_prof_now();
        ended = run(arg);
        st.rerun_ticks += ps3_prof_now() - t;
        in_frame = false;
        sounds_rerun_end(j);
        st.reruns++;
        frame_done(j);
        if (ended) {
            *end = j;
            rerunning = false;
            return 1;
        }
    }
    rerunning = false;
    return 0;
}

// The scene ended in frame f. It ends once f's remote input is known and the end stands; when a prediction was
// wrong, the frames run again and the scene may go on.
static bool settle(s32 f, Ps3RbFrame run, void* arg)
{
    Ps3NetTick t;
    s32 end;
    int r;

    for (;;) {
        if (!ps3_net_wait(f, &t) || t.rollback_from > f) {
            return true;
        }
        r = net_rerun(t.rollback_from, f + 1, t.confirmed, run, arg, &end);
        if (r < 0) {
            net_fail();
            return true;
        }
        if (r == 0) {
            OSReport("rollback: the scene end at frame %d came from a wrong prediction; the match goes on\n", f);
            frame = f + 1;
            return false;
        }
        f = end;
        frame = end + 1;
    }
}

// Netplay: the remote input of a frame may be a prediction; when the real one differs, the frames from there run
// again. A tick that runs no frame (time sync, waits) drops its pad queue entry.
static bool net_frame(Ps3RbFrame run, void* arg)
{
    Ps3NetTick t;
    uint64_t t0;
    s32 end;
    bool ended;
    int r;

    ps3_net_tick(frame, &t);
    // The tick has checked the frames up to the wrong one: they run again even when this tick runs no new frame.
    if (t.rollback_from < frame) {
        r = net_rerun(t.rollback_from, frame, t.confirmed, run, arg, &end);
        if (r < 0) {
            net_fail();
            return run(arg);
        }
        if (r > 0) {
            HSD_PadDropQueueEntry();
            frame = end + 1;
            return settle(end, run, arg);
        }
    }
    if (t.stall) {
        HSD_PadDropQueueEntry();
        return false;
    }
    t0 = ps3_prof_now();
    if (frame > t.confirmed && !save(frame)) {
        net_fail();
        return run(arg);
    }
    st.save_ticks += ps3_prof_now() - t0;
    sounds_first(frame);
    in_frame = true;
    ended = run(arg);
    in_frame = false;
    frame_done(frame);
    frame++;
    return ended && settle(frame - 1, run, arg);
}

void ps3_rb_log_view(const char* whose, int f, const void* data, int size)
{
    static const char* const grounds[] = { "ground", "air" };
    Gameplay g;
    int i;

    if (size != (int) sizeof(g)) {
        OSReport("rollback: %s's gameplay values have %d bytes, not %u\n", whose, size, (u32) sizeof(g));
        return;
    }
    memcpy(&g, data, sizeof(g));
    OSReport("rollback: %s, frame %d: RNG seed %08x, stocks %d %d %d %d\n", whose, f, g.seed, g.stocks[0],
             g.stocks[1], g.stocks[2], g.stocks[3]);
    for (i = 0; i < 12; i++) {
        const FighterView* v = &g.f[i];

        if (v->present) {
            OSReport("rollback: %s, frame %d: fighter %d.%d motion %u %s, x %g y %g, velocity %g %g, facing %g, "
                     "animation frame %g, percent %g, hitlag %g, shield %g\n",
                     whose, f, i / 2, i % 2, v->motion, grounds[v->ground & 1], (double) v->pos[0],
                     (double) v->pos[1], (double) v->vel[0], (double) v->vel[1], (double) v->facing,
                     (double) v->anim, (double) v->percent, (double) v->hitlag, (double) v->shield);
        }
    }
}

int ps3_rb_sound_rerun(int id)
{
    FrameSounds* fs = &sounds[rerun_frame % INPUTS];
    int i;

    for (i = 0; i < fs->n; i++) {
        if (!fs->s[i].claimed && fs->s[i].id == id) {
            fs->s[i].claimed = true;
            return fs->s[i].vid;
        }
    }
    return -1;
}

void ps3_rb_sound_started(int id, int vid)
{
    FrameSounds* fs = &sounds[frame % INPUTS];

    if (active && in_frame && !rerunning && vid >= 0 && fs->frame == frame && fs->n < FRAME_SOUNDS) {
        fs->s[fs->n].id = id;
        fs->s[fs->n].vid = vid;
        fs->s[fs->n].claimed = false;
        fs->n++;
    }
}

typedef struct Region {
    const char* name;
    u8* p;
    size_t n;
} Region;

static uint32_t* hashes;
static size_t nhashes;

// What part of the state, if any, holds an arena address.
static const char* arena_part(uintptr_t a)
{
    int count, i;
    PsHeap* table = ps3_heap_table(&count);

    if (a >= (uintptr_t) table && a < (uintptr_t) (table + count)) {
        return "heap table (saved)";
    }
    if (a >= hsd_heap->start && a < hsd_heap->top) {
        return "HSD heap (saved)";
    }
    if (a >= hsd_heap->top && a < heap_end) {
        return "HSD heap above its top";
    }
    for (i = 0; i < count; i++) {
        if (table[i].size >= 0 && a >= table[i].start && a < table[i].start + (uintptr_t) table[i].size) {
            return "another OS heap";
        }
    }
    return "outside the OS heaps";
}

static void log_run(const Region* region, size_t from, size_t to, const char* part, int* lines)
{
    if (part != NULL && (*lines)++ < 40) {
        OSReport("rollback: survey: %s +%#x..+%#x (%u KB) changed: %s\n", region->name, (u32) (from * CHUNK),
                 (u32) (to * CHUNK), (u32) ((to - from) * CHUNK >> 10), part);
    }
}

// Hashes each page of the arena, the pool and the saved statics; from the second call on, logs the pages that
// changed since the call before, as runs with what part of the state they are.
static void survey(void)
{
    Region regions[4];
    unsigned int arena_size;
    void* dat_pool;
    size_t k = 0;
    bool first = hashes == NULL;
    int r;

    regions[0].name = "arena";
    ps3_os_arena(&regions[0].p, &arena_size);
    regions[0].n = arena_size;
    regions[1].name = ".dat pool";
    ps3_dat_pool_range(&dat_pool, &regions[1].n);
    regions[1].p = dat_pool;
    regions[2].name = "data";
    regions[2].p = (u8*) __rb_data_start;
    regions[2].n = (size_t) (__rb_data_end - __rb_data_start);
    regions[3].name = "bss";
    regions[3].p = (u8*) __rb_bss_start;
    regions[3].n = (size_t) (__rb_bss_end - __rb_bss_start);
    if (first) {
        for (r = 0; r < 4; r++) {
            nhashes += (regions[r].n + CHUNK - 1) / CHUNK;
        }
        hashes = malloc(nhashes * sizeof(*hashes));
        if (hashes == NULL) {
            OSReport("rollback: no memory for the survey\n");
            return;
        }
    }
    OSReport("rollback: survey at frame %d: HSD heap %p, top +%u KB of %u KB\n", frame, (void*) hsd_heap->start,
             (u32) ((hsd_heap->top - hsd_heap->start) >> 10), (u32) ((heap_end - hsd_heap->start) >> 10));
    for (r = 0; r < 4; r++) {
        size_t pages = (regions[r].n + CHUNK - 1) / CHUNK, i, changed = 0, run_start = 0;
        const char* run_part = NULL;
        int lines = 0;

        for (i = 0; i < pages; i++, k++) {
            const u8* p = regions[r].p + i * CHUNK;
            size_t len = regions[r].n - i * CHUNK < CHUNK ? regions[r].n - i * CHUNK : CHUNK;
            uint32_t h = rb_hash(p, (u32) len);
            const char* part = NULL;

            if (!first && hashes[k] != h) {
                part = r == 0 ? arena_part((uintptr_t) p) : r == 1 ? "the pool" : "statics (saved)";
            }
            hashes[k] = h;
            changed += part != NULL;
            if (part != run_part) {
                log_run(&regions[r], run_start, i, run_part, &lines);
                run_part = part;
                run_start = i;
            }
        }
        log_run(&regions[r], run_start, pages, run_part, &lines);
        if (!first) {
            OSReport("rollback: survey: %s: %u of %u pages changed\n", regions[r].name, (u32) changed, (u32) pages);
        }
    }
}

// Lays the store out in the block, each part 128-byte aligned for the SPU's DMA: the tables, the hash rows, the undo
// log, and the shadow with a CHUNK slot per chunk.
static size_t layout(u8* base)
{
    size_t off = 0, need;
    u32 c = 0;
    int i;

#define TAKE(ptr, type, count)                                                                                      \
    need = ((size_t) (count) * sizeof(type) + 127) & ~(size_t) 127;                                                  \
    if (base != NULL) {                                                                                              \
        ptr = (type*) (base + off);                                                                                  \
    }                                                                                                                \
    off += need
    TAKE(chunks, RbChunk, nchunks);
    TAKE(stamp, u32, nchunks);
    TAKE(out_buf, u32, nchunks + 4);
    TAKE(apply_buf, RbApply, POOL_CHUNKS);
    TAKE(log_chunk, u32, POOL_CHUNKS);
    TAKE(ref_hashes, u32, (size_t) MAX_DEPTH * nchunks);
    TAKE(now_hashes, u32, nchunks);
    TAKE(pool, u8, (size_t) POOL_CHUNKS * CHUNK);
    TAKE(shadow, u8, shadow_size);
#undef TAKE
    if (base != NULL) {
        for (i = 0; i < NRANGES; i++) {
            size_t o;

            for (o = 0; o < ranges[i].n; o += CHUNK, c++) {
                chunks[c].mem_ea = (u32) (uintptr_t) (ranges[i].p + o);
                chunks[c].shadow_ea = (u32) (uintptr_t) (shadow + (size_t) c * CHUNK);
                chunks[c].len = (u32) (ranges[i].n - o < CHUNK ? ranges[i].n - o : CHUNK);
                chunks[c].pad = 0;
                stamp[c] = 0;
            }
        }
    }
    return off;
}

static void start(void)
{
    int count, h = HSD_GetHeap(), i;
    PsHeap* table = ps3_heap_table(&count);
    uintptr_t limit;
    size_t size;
    u32 c;

    armed = false;
    if (h < 0 || h >= count || table[h].size < 0) {
        OSReport("rollback: no HSD heap (%d)\n", h);
        return;
    }
    hsd_heap = &table[h];
    heap_end = hsd_heap->start + (uintptr_t) hsd_heap->size;
    // Never allocated in this scene: zeros, so a restore can give blocks above the saved top back as they were.
    memset((void*) hsd_heap->top, 0, heap_end - hsd_heap->top);
    ps3_mem_written((void*) hsd_heap->top, (u32) (heap_end - hsd_heap->top));
    if (mode == MODE_SYNC || net) {
        if (spu_state < 0) {
            spu_state = rb_start != NULL && rb_start(&rb_sh) ? 1 : 0;
        }
        limit = hsd_heap->top + HEAP_ROOM < heap_end ? hsd_heap->top + HEAP_ROOM : heap_end;
        // DMA sizes are multiples of 16: the statics' ends are 128-byte aligned, the table is followed by padding.
        ranges[0].p = (u8*) __rb_data_start;
        ranges[0].n = (size_t) (__rb_data_end - __rb_data_start);
        ranges[1].p = (u8*) __rb_bss_start;
        ranges[1].n = (size_t) (__rb_bss_end - __rb_bss_start);
        ranges[2].p = (u8*) table;
        ranges[2].n = ((size_t) count * sizeof(PsHeap) + 15) & ~(size_t) 15;
        ranges[3] = cover;
        ranges[R_HEAP].p = (u8*) hsd_heap->start;
        ranges[R_HEAP].n = (limit - hsd_heap->start) & ~(uintptr_t) 15;
        nchunks = 0;
        for (i = 0; i < NRANGES; i++) {
            range_first[i] = nchunks;
            nchunks += (u32) ((ranges[i].n + CHUNK - 1) / CHUNK);
        }
        range_first[NRANGES] = nchunks;
        shadow_size = (size_t) nchunks * CHUNK;
        size = layout(NULL);
        block = ps_heap_alloc_high(hsd_heap, size);
        if (block == NULL || (uintptr_t) block < limit) {
            OSReport("rollback: no room for a %u KB store above the HSD heap's %u KB and %u KB to grow\n",
                     (u32) (size >> 10), (u32) ((hsd_heap->top - hsd_heap->start) >> 10), HEAP_ROOM >> 10);
            if (block != NULL) {
                ps_heap_free(hsd_heap, block);
                block = NULL;
            }
            if (net) {
                OSReport("rollback: no netplay without the store; the pairing ends\n");
                ps3_net_cancel();
                net = false;
            }
            return;
        }
        layout(block);
        // The reservation changed the heap's free list: the shadow copies the state after it.
        for (c = 0; c < nchunks; c++) {
            memcpy(SHADOW(c), MEM(c), LEN(c));
        }
        head = tail = 0;
        epoch = 0;
        oldest = shadow_frame = 0;
        frame_start[0] = 0;
        for (i = 0; i < MAX_DEPTH; i++) {
            ref_frame[i] = -1;
        }
    }
    ps3_dat_pool_defer(true);
    active = true;
    ended_log = false;
    frame = 0;
    diff_frames = play_frames = 0;
    memset(&st, 0, sizeof(st));
    memset(&last, 0, sizeof(last));
    if (net) {
        ps3_net_session_begin();
        OSReport("rollback: netplay session on the %s: HSD heap %u KB used of %u KB; store %u KB in the HSD heap, %u "
                 "MB free\n",
                 spu_state == 1 ? "SPU" : "PPE", (u32) ((hsd_heap->top - hsd_heap->start) >> 10),
                 (u32) ((heap_end - hsd_heap->start) >> 10), (u32) (size >> 10), ps3_sys_free_mb());
    } else if (mode == MODE_SYNC) {
        OSReport("rollback: session, sync test (%d frames run again every %d) on the %s: data %u KB, bss %u KB, "
                 "buffer %u KB, HSD heap %u KB used of %u KB; store %u KB in the HSD heap (shadow %u KB, undo log %u "
                 "KB), %u MB free\n",
                 depth, every, spu_state == 1 ? "SPU" : "PPE", (u32) (ranges[0].n >> 10), (u32) (ranges[1].n >> 10),
                 (u32) (ranges[3].n >> 10), (u32) ((hsd_heap->top - hsd_heap->start) >> 10),
                 (u32) ((heap_end - hsd_heap->start) >> 10),
                 (u32) (size >> 10), (u32) (shadow_size >> 10), (POOL_CHUNKS * CHUNK) >> 10, ps3_sys_free_mb());
    } else {
        OSReport("rollback: session, survey: HSD heap %u KB used of %u KB\n",
                 (u32) ((hsd_heap->top - hsd_heap->start) >> 10), (u32) ((heap_end - hsd_heap->start) >> 10));
    }
}

static void stop(void)
{
    if (active) {
        OSReport("rollback: session end at frame %d: %u saves, %u restores, %u frames run again; memory differed %u "
                 "times, gameplay %u times\n",
                 frame, st.saves, st.restores, st.reruns, st.differ, st.play_differ);
        ps3_dat_pool_defer(false);
        ended_log = true;
    }
    if (net) {
        ps3_net_session_end(frame);
        net = false;
    }
    if (block != NULL) {
        ps_heap_free(hsd_heap, block);
        block = NULL;
    }
    free(hashes);
    hashes = NULL;
    nhashes = 0;
    cover.p = NULL;
    cover.n = 0;
    active = armed = rerunning = false;
}

void ps3_rb_match_start(void)
{
    if (mode < 0) {
        read_mode();
    }
    net = ps3_net_session();
    armed = net || mode != MODE_OFF;
}

void ps3_rb_scene_end(void)
{
    stop();
}

bool ps3_rb_frame(Ps3RbFrame run, void* arg)
{
    bool ended;

    if (armed) {
        start();
    }
    if (!active) {
        return run(arg);
    }
    if (net) {
        return net_frame(run, arg);
    }
    if (mode == MODE_SYNC) {
        return sync_frame(run, arg);
    }
    if (frame >= SURVEY_FIRST && frame <= SURVEY_LAST && (frame - SURVEY_FIRST) % SURVEY_STEP == 0) {
        survey();
    }
    ended = run(arg);
    frame++;
    return ended;
}

bool ps3_rb_pad(PADStatus stat[4], bool have)
{
    Input* in;

    if (!active) {
        return have;
    }
    if (net) {
        ps3_net_frame_inputs(rerunning ? rerun_frame : frame, !rerunning && have, stat);
        return true;
    }
    if (rerunning) {
        in = &inputs[rerun_frame % INPUTS];
        if (in->have) {
            memcpy(stat, in->stat, sizeof(in->stat));
        }
        return in->have;
    }
    in = &inputs[frame % INPUTS];
    in->have = have;
    if (have) {
        memcpy(in->stat, stat, sizeof(in->stat));
    }
    return have;
}

bool ps3_rb_rerun(void)
{
    return rerunning;
}

bool ps3_rb_session(void)
{
    return active;
}

void ps3_rb_cover(void* p, size_t n)
{
    if (((uintptr_t) p & 15) != 0) {
        OSReport("rollback: buffer %p is not 16-byte aligned; not saved\n", p);
        return;
    }
    cover.p = p;
    cover.n = n & ~(size_t) 15;
}

void ps3_rb_trace_state(int* f, bool* rerun, bool* inside, unsigned* pass)
{
    *f = rerunning ? rerun_frame : frame;
    *rerun = rerunning;
    *inside = in_frame;
    *pass = st.restores;
}

void ps3_rb_log(unsigned n)
{
    Stats d;

    if (!active && !ended_log) {
        return;
    }
    d.saves = st.saves - last.saves;
    d.restores = st.restores - last.restores;
    d.reruns = st.reruns - last.reruns;
    d.differ = st.differ - last.differ;
    d.play_differ = st.play_differ - last.play_differ;
    d.chunks = st.chunks - last.chunks;
    d.applied = st.applied - last.applied;
    d.save_ticks = st.save_ticks - last.save_ticks;
    d.restore_ticks = st.restore_ticks - last.restore_ticks;
    d.rerun_ticks = st.rerun_ticks - last.rerun_ticks;
    d.hash_ticks = st.hash_ticks - last.hash_ticks;
    d.chunks_max = st.chunks_max;
    st.chunks_max = 0;
    last = st;
    ended_log = false;
    OSReport("heartbeat %u: rollback: frame %d; %u saves (%u us, %u chunks each, at most %u), %u restores (%u us, "
             "%u chunks each), %u frames run again (%u us each), hashes %u us a second; differed: memory %u, gameplay "
             "%u; undo log %u of %u chunks, heap top +%u KB\n",
             n, frame, d.saves, d.saves != 0 ? us(d.save_ticks) / d.saves : 0, d.saves != 0 ? d.chunks / d.saves : 0,
             d.chunks_max, d.restores, d.restores != 0 ? us(d.restore_ticks) / d.restores : 0,
             d.restores != 0 ? d.applied / d.restores : 0, d.reruns, d.reruns != 0 ? us(d.rerun_ticks) / d.reruns : 0,
             us(d.hash_ticks), d.differ, d.play_differ, block != NULL ? head - tail : 0, POOL_CHUNKS,
             hsd_heap != NULL ? (u32) ((hsd_heap->top - hsd_heap->start) >> 10) : 0);
}
