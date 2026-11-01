// The main thread's frame phases (--wrap on the scene loop's calls) and a 250 Hz sampler of what it runs: the GObj
// proc or render callback, else the phase. The heartbeat logs both (mainprof.h).
#include "mainprof.h"

#include <stdio.h>
#include <string.h>

#include <dolphin/os.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/video.h>

#include "diag.h"
#include "prof.h"
#include "sync.h"

void __real_HSD_GObj_RunProcs(void);
void __real_HSD_GObj_80390FC0(void);
void __real_HSD_VICopyXFBAsync(HSD_RenderPass rpass);

#define SLOTS 512
#define TOP 12

typedef struct Slot {
    uintptr_t key;
    u32 count;
} Slot;

static volatile u32 phase;
static Slot slots[SLOTS];
static u32 phase_samples[MAIN_PHASE_COUNT];
static u32 samples;
static PsMutex lock = PS_MUTEX_INIT;

void __wrap_HSD_GObj_RunProcs(void)
{
    uint64_t t0 = ps3_prof_now(), t = t0;

    phase = MAIN_PHASE_LOGIC;
    __real_HSD_GObj_RunProcs();
    phase = MAIN_PHASE_OTHER;
    ps3_prof_lap(PROF_MAIN_LOGIC, &t);
    ps3_prof.logic_frames++;
    if (t - t0 > ps3_prof.logic_max) {
        ps3_prof.logic_max = t - t0;
    }
}

void __wrap_HSD_GObj_80390FC0(void)
{
    uint64_t t0 = ps3_prof_now(), t = t0;

    phase = MAIN_PHASE_RENDER;
    __real_HSD_GObj_80390FC0();
    phase = MAIN_PHASE_OTHER;
    ps3_prof_lap(PROF_MAIN_RENDER, &t);
    ps3_prof.render_passes++;
    if (t - t0 > ps3_prof.render_max) {
        ps3_prof.render_max = t - t0;
    }
}

void __wrap_HSD_VICopyXFBAsync(HSD_RenderPass rpass)
{
    uint64_t t = ps3_prof_now();

    phase = MAIN_PHASE_XFB;
    __real_HSD_VICopyXFBAsync(rpass);
    phase = MAIN_PHASE_OTHER;
    ps3_prof_lap(PROF_MAIN_XFB, &t);
}

static void count(uintptr_t key)
{
    u32 i = (u32) ((key >> 3) * 2654435761u) % SLOTS;
    u32 n;

    for (n = 0; n < SLOTS; n++, i = (i + 1) % SLOTS) {
        if (slots[i].key == key || slots[i].key == 0) {
            slots[i].key = key;
            slots[i].count++;
            return;
        }
    }
}

// A freed proc or GObj stays readable memory; a stale callback only skews the counts.
static void sample(void)
{
    u32 p = phase;
    uintptr_t key = 0;

    if (p == MAIN_PHASE_LOGIC) {
        HSD_GObjProc* proc = HSD_GObj_CurrentInvokedProc;

        if (proc != NULL) {
            key = (uintptr_t) proc->on_invoke;
        }
    } else if (p == MAIN_PHASE_RENDER) {
        HSD_GObj* gobj = HSD_GObj_804D7814;

        if (gobj != NULL) {
            key = (uintptr_t) gobj->render_cb;
        }
    }
    ps_mutex_lock(&lock);
    samples++;
    phase_samples[p]++;
    // Keys 1 to MAIN_PHASE_COUNT are the phases with no callback running; 0 marks a free slot.
    count(key != 0 ? key : p + 1);
    ps_mutex_unlock(&lock);
}

static void sampler_main(void* arg)
{
    static PsMutex wait_lock = PS_MUTEX_INIT;
    static PsCond cond = PS_COND_INIT;

    ps_mutex_lock(&wait_lock);
    for (;;) {
        // 250 Hz: each wake preempts the lowest-priority running thread. Off for the GP priority A/B (diag.h), whose
        // GP would be that thread.
        if (ps3_diag.gx_ab & 134217728) {
            ps_cond_timedwait(&cond, &wait_lock, 100000);
            continue;
        }
        ps_cond_timedwait(&cond, &wait_lock, 4000);
        sample();
    }
}

void ps3_mainprof_start(void)
{
    ps_thread_start(sampler_main, NULL, "sampler");
}

void ps3_mainprof_log(u32 n)
{
    static const char* const names[] = { "other", "logic", "render", "XFB" };
    char line[400];
    Slot top[TOP];
    u32 total, ph[MAIN_PHASE_COUNT], i, j;
    int len;

    ps_mutex_lock(&lock);
    memset(top, 0, sizeof(top));
    for (i = 0; i < SLOTS; i++) {
        for (j = 0; j < TOP; j++) {
            if (slots[i].count > top[j].count) {
                memmove(&top[j + 1], &top[j], (TOP - 1 - j) * sizeof(top[0]));
                top[j] = slots[i];
                break;
            }
        }
    }
    total = samples;
    memcpy(ph, phase_samples, sizeof(ph));
    memset(slots, 0, sizeof(slots));
    memset(phase_samples, 0, sizeof(phase_samples));
    samples = 0;
    ps_mutex_unlock(&lock);
    if (total == 0) {
        return;
    }
    len = snprintf(line, sizeof(line), "%u samples", total);
    for (i = 0; i < MAIN_PHASE_COUNT; i++) {
        len += snprintf(line + len, sizeof(line) - (size_t) len, ", %s %u", names[i], ph[i] * 1000 / total);
    }
    len += snprintf(line + len, sizeof(line) - (size_t) len, "; per mille by callback:");
    for (i = 0; i < TOP && top[i].count != 0 && len < (int) sizeof(line) - 24; i++) {
        u32 pm = top[i].count * 1000 / total;

        if (top[i].key <= MAIN_PHASE_COUNT) {
            len += snprintf(line + len, sizeof(line) - (size_t) len, " %s %u", names[top[i].key - 1], pm);
        } else {
            len += snprintf(line + len, sizeof(line) - (size_t) len, " %08x %u", (u32) top[i].key, pm);
        }
    }
    OSReport("heartbeat %u: main sampler: %s\n", n, line);
}
