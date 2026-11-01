// Saved game states and logic frames run again (ps3_rollback.h). A state is the game's statics (rollback_data.ld,
// rollback_bss.ld), the OS heap table and the HSD heap up to its high-water mark.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <melee/ft/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/initialize.h>
#include <sysdolphin/baselib/generator.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/random.h>
#include <sysdolphin/baselib/state.h>

#include <ps3_rollback.h>

#include "../dat/pool.h"
#include "../os/alarm.h"
#include "../os/heap.h"
#include "../os/mem.h"
#include "../os/prof.h"
#include "../os/sys.h"

extern char __rb_data_start[], __rb_data_end[], __rb_bss_start[], __rb_bss_end[];
// The particle lists by link (particle.c), for the host's render diagnostic.
extern HSD_Particle* hsd_804D0908[16];

#define PAGE 4096u
// Frames whose pad channels are kept, more than the deepest re-run.
#define INPUTS 64
#define MAX_DEPTH 8
#define NRANGES 4
#define MAX_KEPT 64
// Difference runs logged per frame, and frames logged per session.
#define DIFF_LINES 24
#define DIFF_FRAMES 16
// The survey hashes every page at these session frames and logs what changed between them.
#define SURVEY_FIRST 120
#define SURVEY_STEP 60
#define SURVEY_LAST 480

enum { MODE_OFF, MODE_SYNC, MODE_SURVEY };

typedef struct Range {
    u8* p;
    size_t n;
} Range;

static const char* const range_names[NRANGES] = { "data", "bss", "heap table", "heap" };

// The ranges where the memory was when the state was saved: the heap's end moves.
typedef struct State {
    u8* buf;
    size_t cap;
    Range ranges[NRANGES];
    s32 frame;
} State;

typedef struct Input {
    PADStatus stat[4];
    bool have;
} Input;

typedef struct Stats {
    u32 saves, restores, reruns, differ, pages, play_differ;
    uint64_t save_ticks, restore_ticks, rerun_ticks;
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

static int mode = -1;
// Sync test: the frames run again after each checked frame, and every how many frames a check comes.
static int depth = 1, every = 1;
static bool armed, active, rerunning, ended_log, in_frame;
// Host only (MELEE_HOST_RB_RENDERDIFF): at the first gameplay difference, what the render had changed before it.
static bool render_diag;
// Host only (MELEE_HOST_RB_FIRST): after each check the match goes on from the first run's state.
static bool first_timeline;
// Host only (MELEE_HOST_RB_HASHAT): the frame whose first run and re-run start get the survey's page hashes.
static s32 hash_at = -1;
static s32 frame, rerun_frame;
static Input inputs[INPUTS];
static Gameplay plays[INPUTS];
static u32 play_frames;
static State pre[MAX_DEPTH], post[MAX_DEPTH];
static PsHeap* hsd_heap;
static uintptr_t heap_end;
static size_t state_bytes;
static u32 diff_frames;
static Stats st, last;

typedef struct Kept {
    OSAlarm* at;
    OSAlarm bytes;
} Kept;

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
    render_diag = getenv("MELEE_HOST_RB_RENDERDIFF") != NULL;
    first_timeline = getenv("MELEE_HOST_RB_FIRST") != NULL;
    hash_at = getenv("MELEE_HOST_RB_HASHAT") != NULL ? atoi(getenv("MELEE_HOST_RB_HASHAT")) : -1;
#elif defined(PS3_RB_SYNC)
    static char buf[32];

    snprintf(buf, sizeof(buf), "sync:%d:1", PS3_RB_SYNC);
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

static void state_ranges(Range* r)
{
    int count;
    PsHeap* table = ps3_heap_table(&count);

    r[0].p = (u8*) __rb_data_start;
    r[0].n = (size_t) (__rb_data_end - __rb_data_start);
    r[1].p = (u8*) __rb_bss_start;
    r[1].n = (size_t) (__rb_bss_end - __rb_bss_start);
    r[2].p = (u8*) table;
    r[2].n = (size_t) count * sizeof(PsHeap);
    r[3].p = (u8*) hsd_heap->start;
    r[3].n = hsd_heap->top - hsd_heap->start;
}

static bool save(State* s, s32 f)
{
    Range r[NRANGES];
    size_t total = 0;
    u8* out = s->buf;
    int i;

    state_ranges(r);
    for (i = 0; i < NRANGES; i++) {
        total += r[i].n;
    }
    if (total > s->cap) {
        OSReport("rollback: a state of %u KB does not fit in %u KB\n", (u32) (total >> 10), (u32) (s->cap >> 10));
        return false;
    }
    for (i = 0; i < NRANGES; i++) {
        memcpy(out, r[i].p, r[i].n);
        out += r[i].n;
        s->ranges[i] = r[i];
    }
    s->frame = f;
    state_bytes = total;
    st.saves++;
    return true;
}

// Writes only the pages that differ, and tells the GP's caches about them.
static void copy_back(u8* dst, const u8* src, size_t n)
{
    size_t off = 0, run = 0, run_len = 0;

    while (off < n) {
        size_t chunk = PAGE - ((uintptr_t) (dst + off) & (PAGE - 1));

        chunk = chunk > n - off ? n - off : chunk;
        if (memcmp(dst + off, src + off, chunk) != 0) {
            memcpy(dst + off, src + off, chunk);
            if (run_len == 0) {
                run = off;
            }
            run_len = off + chunk - run;
            st.pages++;
        } else if (run_len != 0) {
            ps3_mem_written(dst + run, (u32) run_len);
            run_len = 0;
        }
        off += chunk;
    }
    if (run_len != 0) {
        ps3_mem_written(dst + run, (u32) run_len);
    }
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

static bool in_kept(const u8* p);

// Host diagnostic: the bytes that differ from the state, kept ones aside.
static size_t differ_bytes(const State* s)
{
    const u8* then = s->buf;
    size_t differ = 0, off;
    int i;

    for (i = 0; i < NRANGES; then += s->ranges[i].n, i++) {
        for (off = 0; off < s->ranges[i].n; off++) {
            differ += s->ranges[i].p[off] != then[off] && !in_kept(s->ranges[i].p + off);
        }
    }
    return differ;
}

static void restore_step(const State* s, bool trace, const char* name)
{
    if (trace) {
        OSReport("rollback: restore, after %s: %u bytes differ\n", name, (u32) differ_bytes(s));
    }
}

static void restore(const State* s, bool trace)
{
    // The heap's top before its table goes back: blocks above the saved top are newer than the state.
    uintptr_t top = hsd_heap->top;
    const u8* in = s->buf;
    BOOL enabled = OSDisableInterrupts();
    int i;

    keep();
    for (i = 0; i < NRANGES; i++) {
        copy_back(s->ranges[i].p, in, s->ranges[i].n);
        in += s->ranges[i].n;
    }
    // The session cleared the heap above its top, so newer blocks go back to zeros.
    if (top > hsd_heap->top) {
        memset((void*) hsd_heap->top, 0, top - hsd_heap->top);
        ps3_mem_written((void*) hsd_heap->top, (u32) (top - hsd_heap->top));
    }
    restore_step(s, trace, "the copy");
    put_back();
    restore_step(s, trace, "put_back");
    OSRestoreInterrupts(enabled);
    restore_step(s, trace, "interrupts on");
    // The restored render caches name GX state of an older frame.
    HSD_StateInvalidate(-1);
    restore_step(s, trace, "HSD_StateInvalidate");
    st.restores++;
}

static u32 word_at(const u8* p, size_t off, size_t n)
{
    u32 w = 0;
    size_t i;

    for (i = 0; i < 4 && off + i < n; i++) {
        w = w << 8 | p[off + i];
    }
    return w;
}

// Bytes that keep() keeps move with the alarm thread or the render, not with the game's logic.
static bool in_kept(const u8* p)
{
    int i;

    for (i = 0; i < nkept; i++) {
        if (p >= (const u8*) kept[i].at && p < (const u8*) (kept[i].at + 1)) {
            return true;
        }
    }
    for (i = 0; i < 2; i++) {
        if (shape_at[i] != NULL && p >= (const u8*) shape_at[i] && p < (const u8*) shape_at[i] + shape_size[i]) {
            return true;
        }
    }
    return false;
}

// The runs of a compare that does not log: the host's render diagnostic prints them at the first gameplay difference.
typedef struct DiffRun {
    const u8* at;
    u32 len, now0, now1, then0, then1;
    int range;
} DiffRun;

static DiffRun pending[1024];
static int npending;

// Fighters by address, for heap runs.
// The live fighter, particle or generator that holds p, or false.
static bool owner_of(const u8* p, char* where, size_t size)
{
    const HSD_Particle* pp;
    const HSD_Generator* gp;
    int i, k;

    for (i = 0; i < 6; i++) {
        for (k = 0; k < 2; k++) {
            HSD_GObj* gobj = Player_GetEntityAtIndex(i, k);
            const u8* fp = gobj != NULL ? gobj->user_data : NULL;

            if (fp != NULL && p >= fp && p < fp + sizeof(Fighter)) {
                snprintf(where, size, "fighter %d.%d +%#x", i, k, (u32) (p - fp));
                return true;
            }
        }
    }
    for (i = 0; i < 16; i++) {
        for (pp = hsd_804D0908[i]; pp != NULL; pp = pp->next) {
            if (p >= (const u8*) pp && p < (const u8*) (pp + 1)) {
                snprintf(where, size, "particle (link %d, kind %#x) +%#x", i, pp->kind, (u32) (p - (const u8*) pp));
                return true;
            }
        }
    }
    for (gp = hsd_804D78FC; gp != NULL; gp = gp->next) {
        if (p >= (const u8*) gp && p < (const u8*) (gp + 1)) {
            snprintf(where, size, "generator (kind %#x, type %#x) +%#x", gp->kind, gp->type, (u32) (p - (const u8*) gp));
            return true;
        }
    }
    return false;
}

// Only the runs in fighters, particles and generators: allocators and caches differ after any render.
static void print_pending(s32 f)
{
    int i, others = 0;

    OSReport("rollback: frame %d: before it ran again, memory differed from its first run in %d runs:\n", f, npending);
    for (i = 0; i < npending; i++) {
        const DiffRun* r = &pending[i];
        char where[96];

        if (!owner_of(r->at, where, sizeof(where))) {
            others++;
            continue;
        }
        OSReport("rollback:   %p (%s), %u bytes: now %08x %08x, first run %08x %08x\n", r->at, where, r->len, r->now0,
                 r->now1, r->then0, r->then1);
    }
    OSReport("rollback:   and %d runs elsewhere\n", others);
}

// Logs where the memory differs from the state saved after the frame's first run. The number of bytes that differ.
// Without log, keeps the runs in pending instead and counts nothing.
static size_t compare(const State* s, s32 f, bool log)
{
    const u8* then = s->buf;
    size_t differ = 0;
    int lines = 0, i;

    npending = 0;

    keep();
    for (i = 0; i < NRANGES; then += s->ranges[i].n, i++) {
        const u8* now = s->ranges[i].p;
        size_t n = s->ranges[i].n, off = 0;

        if (memcmp(now, then, n) == 0) {
            continue;
        }
        while (off < n) {
            size_t start, end, gap;

            if (now[off] == then[off] || in_kept(now + off)) {
                off++;
                continue;
            }
            // A run ends after 16 equal bytes.
            start = off;
            for (end = off, gap = 0; end < n && gap < 16; end++) {
                if (now[end] != then[end] && !in_kept(now + end)) {
                    differ++;
                    gap = 0;
                } else {
                    gap++;
                }
            }
            end -= gap;
            if (!log && npending < (int) (sizeof(pending) / sizeof(pending[0]))) {
                DiffRun* r = &pending[npending++];

                r->at = now + start;
                r->len = (u32) (end - start);
                r->range = i;
                r->now0 = word_at(now, start & ~3, n);
                r->now1 = word_at(now, (start & ~3) + 4, n);
                r->then0 = word_at(then, start & ~3, n);
                r->then1 = word_at(then, (start & ~3) + 4, n);
            } else if (log && lines < DIFF_LINES && diff_frames < DIFF_FRAMES) {
                OSReport("rollback: frame %d: %s +%#x (%p), %u bytes: now %08x %08x, first run %08x %08x\n", f,
                         range_names[i], (u32) start, now + start, (u32) (end - start), word_at(now, start & ~3, n),
                         word_at(now, (start & ~3) + 4, n), word_at(then, start & ~3, n),
                         word_at(then, (start & ~3) + 4, n));
            }
            lines++;
            off = end;
        }
    }
    if (differ != 0 && log) {
        if (diff_frames < DIFF_FRAMES) {
            OSReport("rollback: frame %d: %u bytes differ in %d runs after it ran again\n", f, (u32) differ, lines);
        }
        diff_frames++;
        st.differ++;
    }
    return differ;
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
static void survey(void);

// The sync test: each frame is saved before and after it runs; then the last depth frames run again from the
// oldest saved state. The first must leave all memory as its first run did, each the same gameplay values.
static bool sync_frame(Ps3RbFrame run, void* arg)
{
    s32 f = frame, from, j;
    uint64_t t = ps3_prof_now(), t0;
    bool ended;

    if (!save(&pre[f % depth], f)) {
        stop();
        return run(arg);
    }
    st.save_ticks += ps3_prof_now() - t;
    if (f == hash_at) {
        survey();
    }
    in_frame = true;
    ended = run(arg);
    in_frame = false;
    frame++;
    if (ended) {
        return true;
    }
    view_gameplay(&plays[f % INPUTS]);
    t = ps3_prof_now();
    if (!save(&post[f % depth], f)) {
        stop();
        return false;
    }
    st.save_ticks += ps3_prof_now() - t;
    from = f - depth + 1;
    if (from < 0 || f % every != 0) {
        return false;
    }
    t = ps3_prof_now();
    restore(&pre[from % depth], from == hash_at);
    t0 = ps3_prof_now();
    st.restore_ticks += t0 - t;
    // The pages that differ from the start of that frame's first run, and the restore against its state.
    if (from == hash_at) {
        survey();
        diff_frames = 0;
        compare(&pre[from % depth], from, true);
    }
    rerunning = true;
    for (j = from; j <= f; j++) {
        // What the first run of j started from that this run does not: the render in between, and older re-runs.
        if (render_diag && j > from) {
            compare(&pre[j % depth], j, false);
        }
        rerun_frame = j;
        in_frame = true;
        ended = run(arg);
        in_frame = false;
        st.reruns++;
        if (ended) {
            OSReport("rollback: frame %d ended the scene when it ran again\n", j);
            break;
        }
        t = ps3_prof_now();
        if (j == from) {
            compare(&post[j % depth], j, true);
        }
        if (compare_gameplay(&plays[j % INPUTS], j, j - from) && render_diag && j > from && play_frames == 1) {
            print_pending(j);
        }
        t0 += ps3_prof_now() - t;
    }
    rerunning = false;
    st.rerun_ticks += ps3_prof_now() - t0;
    // Host diagnostic: back on the first run's timeline, so a difference does not carry into the next checks.
    if (first_timeline && !ended) {
        restore(&post[f % depth], false);
    }
    return ended;
}

typedef struct Region {
    const char* name;
    u8* p;
    size_t n;
} Region;

static uint32_t* hashes;
static size_t nhashes;

static uint32_t hash_page(const u8* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;

    for (i = 0; i + 8 <= n; i += 8) {
        uint64_t w;

        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return (uint32_t) (h ^ h >> 32);
}

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
        OSReport("rollback: survey: %s +%#x..+%#x (%u KB) changed: %s\n", region->name, (u32) (from * PAGE),
                 (u32) (to * PAGE), (u32) ((to - from) * PAGE >> 10), part);
    }
}

// Hashes each page of the arena, the pool and the saved statics; from the second call on, logs the pages that
// changed since the call before, as runs with what part of the state they are.
static void survey(void)
{
    Region regions[4];
    unsigned int arena_size;
    void* pool;
    size_t k = 0;
    bool first = hashes == NULL;
    int r;

    regions[0].name = "arena";
    ps3_os_arena(&regions[0].p, &arena_size);
    regions[0].n = arena_size;
    regions[1].name = ".dat pool";
    ps3_dat_pool_range(&pool, &regions[1].n);
    regions[1].p = pool;
    regions[2].name = "data";
    regions[2].p = (u8*) __rb_data_start;
    regions[2].n = (size_t) (__rb_data_end - __rb_data_start);
    regions[3].name = "bss";
    regions[3].p = (u8*) __rb_bss_start;
    regions[3].n = (size_t) (__rb_bss_end - __rb_bss_start);
    if (first) {
        for (r = 0; r < 4; r++) {
            nhashes += (regions[r].n + PAGE - 1) / PAGE;
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
        size_t pages = (regions[r].n + PAGE - 1) / PAGE, i, changed = 0, run_start = 0;
        const char* run_part = NULL;
        int lines = 0;

        for (i = 0; i < pages; i++, k++) {
            const u8* p = regions[r].p + i * PAGE;
            size_t len = regions[r].n - i * PAGE < PAGE ? regions[r].n - i * PAGE : PAGE;
            uint32_t h = hash_page(p, len);
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

static void start(void)
{
    int count, h = HSD_GetHeap(), i;
    PsHeap* table = ps3_heap_table(&count);
    size_t cap;

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
    cap = (size_t) (__rb_data_end - __rb_data_start) + (size_t) (__rb_bss_end - __rb_bss_start) +
          (size_t) count * sizeof(PsHeap) + (hsd_heap->top - hsd_heap->start);
    // Room for the heap to grow during the match: it grew about 230 KB in host matches.
    cap += (heap_end - hsd_heap->top) < (2u << 20) ? heap_end - hsd_heap->top : 2u << 20;
    if (mode == MODE_SYNC) {
        // On the PS3 an allocation over the free memory can hang lv2 instead of failing.
        unsigned need_mb = (unsigned) ((2 * (size_t) depth * cap) >> 20) + 1, free_mb = ps3_sys_free_mb();

        if (need_mb + 16 > free_mb) {
            OSReport("rollback: %d states of %u KB need %u MB, %u MB free\n", 2 * depth, (u32) (cap >> 10), need_mb,
                     free_mb);
            return;
        }
        for (i = 0; i < depth; i++) {
            pre[i].buf = malloc(cap);
            post[i].buf = malloc(cap);
            pre[i].cap = post[i].cap = cap;
            if (pre[i].buf == NULL || post[i].buf == NULL) {
                OSReport("rollback: no memory for %d states of %u KB\n", 2 * depth, (u32) (cap >> 10));
                stop();
                return;
            }
        }
    }
    ps3_dat_pool_defer(true);
    active = true;
    ended_log = false;
    frame = 0;
    diff_frames = play_frames = 0;
    memset(&st, 0, sizeof(st));
    memset(&last, 0, sizeof(last));
    OSReport("rollback: session, %s: data %u KB, bss %u KB, heap table %u bytes, HSD heap %u KB used of %u KB; "
             "%u KB a state\n",
             mode == MODE_SYNC ? "sync test" : "survey", (u32) ((__rb_data_end - __rb_data_start) >> 10),
             (u32) ((__rb_bss_end - __rb_bss_start) >> 10), (u32) (count * sizeof(PsHeap)),
             (u32) ((hsd_heap->top - hsd_heap->start) >> 10), (u32) ((heap_end - hsd_heap->start) >> 10),
             (u32) (cap >> 10));
    if (mode == MODE_SYNC) {
        OSReport("rollback: sync test: %d frames run again every %d frames\n", depth, every);
    }
}

static void stop(void)
{
    int i;

    if (active) {
        OSReport("rollback: session end at frame %d: %u saves, %u restores, %u frames run again; memory differed %u "
                 "times, gameplay %u times\n",
                 frame, st.saves, st.restores, st.reruns, st.differ, st.play_differ);
        ps3_dat_pool_defer(false);
        ended_log = true;
    }
    for (i = 0; i < MAX_DEPTH; i++) {
        free(pre[i].buf);
        free(post[i].buf);
        pre[i].buf = post[i].buf = NULL;
    }
    free(hashes);
    hashes = NULL;
    nhashes = 0;
    active = armed = rerunning = false;
}

void ps3_rb_match_start(void)
{
    if (mode < 0) {
        read_mode();
    }
    armed = mode != MODE_OFF;
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
    d.pages = st.pages - last.pages;
    d.save_ticks = st.save_ticks - last.save_ticks;
    d.restore_ticks = st.restore_ticks - last.restore_ticks;
    d.rerun_ticks = st.rerun_ticks - last.rerun_ticks;
    last = st;
    ended_log = false;
    OSReport("heartbeat %u: rollback: frame %d; %u saves (%u us each), %u restores (%u us each, %u pages), %u frames "
             "run again (%u us each); differed: memory %u, gameplay %u; a state %u KB\n",
             n, frame, d.saves, d.saves != 0 ? us(d.save_ticks) / d.saves : 0, d.restores,
             d.restores != 0 ? us(d.restore_ticks) / d.restores : 0, d.pages, d.reruns,
             d.reruns != 0 ? us(d.rerun_ticks) / d.reruns : 0, d.differ, d.play_differ, (u32) (state_bytes >> 10));
}
