// The game's RNG calls and their callers, for the rollback's sync test on the host (EXTRA_SRCS, with
// EXTRA_LDFLAGS=-Wl,--wrap=HSD_Rand,--wrap=HSD_Randf,--wrap=HSD_Randi). A re-run frame whose calls differ from its
// first run's logs both lists; MELEE_HOST_RB_TRACE="from:to" also logs every call of those frames.
#include <stdio.h>
#include <stdlib.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/random.h>

#include <ps3_rollback.h>

s32 __real_HSD_Rand(void);
f32 __real_HSD_Randf(void);
s32 __real_HSD_Randi(s32 max_val);

#define FRAMES 64
#define CALLS 256
#define REPORTS 12

typedef struct Calls {
    int frame, n;
    void* caller[CALLS];
} Calls;

static Calls first[FRAMES];
static int rerun_frame = -1, rerun_n, reports;
static unsigned rerun_pass;
static bool rerun_bad;

static void report(const Calls* c, int f, int at, void* caller)
{
    int i;

    if (reports++ >= REPORTS) {
        return;
    }
    OSReport("rand: frame %d runs again with call %d from %p; the first run made %d:\n", f, at, caller, c->n);
    for (i = 0; i < c->n; i++) {
        OSReport("rand:   %d %p\n", i, c->caller[i]);
    }
}

static void trace(const char* what, void* caller)
{
    static int from = -2, to;
    bool rerun, inside;
    unsigned pass;
    Calls* c;
    int f;

    if (from == -2) {
        const char* s = getenv("MELEE_HOST_RB_TRACE");

        from = -1;
        if (s != NULL) {
            sscanf(s, "%d:%d", &from, &to);
        }
    }
    if (!ps3_rb_session()) {
        return;
    }
    ps3_rb_trace_state(&f, &rerun, &inside, &pass);
    if ((from >= 0 && f >= from && f <= to) || !inside) {
        OSReport("rand: frame %d %s, %s: %s from %p, seed %08x\n", f, rerun ? "again" : "first",
                 inside ? "logic" : "between frames", what, caller, *HSD_RandSeedPtr);
    }
    if (!inside) {
        return;
    }
    c = &first[f % FRAMES];
    if (!rerun) {
        if (c->frame != f) {
            c->frame = f;
            c->n = 0;
        }
        if (c->n < CALLS) {
            c->caller[c->n++] = caller;
        }
        return;
    }
    // Each frame runs again in depth passes, one per restore.
    if (rerun_frame != f || rerun_pass != pass) {
        rerun_frame = f;
        rerun_pass = pass;
        rerun_n = 0;
        rerun_bad = false;
    }
    if (!rerun_bad && c->frame == f && (rerun_n >= c->n || c->caller[rerun_n] != caller)) {
        rerun_bad = true;
        report(c, f, rerun_n, caller);
    }
    rerun_n++;
}

s32 __wrap_HSD_Rand(void)
{
    trace("HSD_Rand", __builtin_return_address(0));
    return __real_HSD_Rand();
}

f32 __wrap_HSD_Randf(void)
{
    trace("HSD_Randf", __builtin_return_address(0));
    return __real_HSD_Randf();
}

s32 __wrap_HSD_Randi(s32 max_val)
{
    trace("HSD_Randi", __builtin_return_address(0));
    return __real_HSD_Randi(max_val);
}
