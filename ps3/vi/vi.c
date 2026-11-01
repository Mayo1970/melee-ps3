// The SDK's VI calls on the PS3 display. Each vertical blank raises the retrace interrupt: the game's callbacks
// run around it, and a flushed frame buffer goes to the GP thread to be shown (the RSX belongs to that thread).
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "../gx/backend.h"
#include "../gx/gp.h"
#include "../gx/rsxdev.h"
#include "../os/diag.h"
#include "../os/intr.h"
#include "../os/mainprof.h"
#include "../os/prof.h"
#include "../os/sync.h"
#include "../os/sys.h"

#define GP_RING_SIZE (1u << 20)

static VIRetraceCallback pre_callback;
static VIRetraceCallback post_callback;
static OSThreadQueue retrace_queue;
static volatile u32 retrace_count;
static void* next_buffer;
static bool flush_pending;
static bool black = true;

static void raise_retrace(void)
{
    ps3_diag.vblanks++;
    ps3_intr_raise(PS3_INTR_VI);
}

static void retrace(void)
{
    u32 count = ++retrace_count;

    if (count == 1) {
        OSReport("VI: first retrace\n");
    }
    ps3_diag.intr_site = "VI pre-retrace callback";
    if (pre_callback != NULL) {
        pre_callback(count);
    }
    ps3_diag.intr_site = "VI present";
    if (flush_pending) {
        flush_pending = false;
        if (!black && next_buffer != NULL) {
            gp_post_present(OSCachedToPhysical(next_buffer));
        }
    }
    ps3_diag.intr_site = "VI post-retrace callback";
    if (post_callback != NULL) {
        post_callback(count);
    }
    ps3_diag.intr_site = "VI wakeup";
    OSWakeupThread(&retrace_queue);
    ps3_diag.intr_site = "VI XMB poll";
    if (ps3_sys_poll()) {
        gp_stop();
        ps3_sys_exit();
    }
}

// The call trace build's main thread: its open calls (outermost first) and its last function entries.
static void log_calltrace(u32 n)
{
    char line[PS3_CALLTRACE_DEPTH * 9 + 1];
    unsigned calls = ps3_diag.main_calls, depth = ps3_diag.main_depth;
    unsigned kept = depth < PS3_CALLTRACE_DEPTH ? depth : PS3_CALLTRACE_DEPTH;
    unsigned i, len = 0;

    // The innermost 40 fit OSReport's 512 bytes.
    for (i = kept > 40 ? kept - 40 : 0; i < kept; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %08x", (u32) (uintptr_t) ps3_diag.main_stack[i]);
    }
    line[len] = '\0';
    OSReport("heartbeat %u: main %u calls, depth %u, stack%s\n", n, calls, depth, line);
    len = 0;
    for (i = 0; i < PS3_CALLTRACE_RECENT; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %08x",
                        (u32) (uintptr_t) ps3_diag.main_recent[(calls + i) % PS3_CALLTRACE_RECENT]);
    }
    line[len] = '\0';
    OSReport("heartbeat %u: main last calls%s\n", n, line);
    if (ps3_diag.watch_hit) {
        kept = ps3_diag.watch_depth < PS3_CALLTRACE_DEPTH ? ps3_diag.watch_depth : PS3_CALLTRACE_DEPTH;
        len = 0;
        for (i = kept > 32 ? kept - 32 : 0; i < kept; i++) {
            len += snprintf(line + len, sizeof(line) - len, " %08x", (u32) (uintptr_t) ps3_diag.watch_stack[i]);
        }
        line[len] = '\0';
        OSReport("heartbeat %u: watch %p became %#llx at fn %08x site %08x on %s; main stack%s\n", n,
                 (void*) ps3_diag.watch, ps3_diag.watch_seen, (u32) (uintptr_t) ps3_diag.watch_fn,
                 (u32) (uintptr_t) ps3_diag.watch_site, ps3_diag.watch_on_main ? "main" : "another thread", line);
    }
}

// The share of the last second each GP and main thread phase took (prof.h), and the draw counts behind them.
static void log_profile(u32 n, const GpStats* gs, const GxRsxStats* rs)
{
    static uint64_t last_now, last[PROF_COUNT], last_bytes;
    static u32 last_draws, last_batchable, last_reuses, last_reloads, last_vp, last_uploads, last_presents;
    static u32 last_batched, last_rsx, last_hits, last_builds, last_runs, last_evictions, last_tex_hashes;
    static u32 last_tex_evictions, last_tex_waits, last_tex_failures, last_indirect, last_tev_setups, last_tev_builds;
    static u32 last_spu_jobs, last_spu_ppe, last_spu_waits;
    static uint64_t last_spu_ticks;
    uint64_t now = ps3_prof_now(), d[PROF_COUNT], span = now - last_now, other;
    u32 pm[PROF_COUNT], i;

    for (i = 0; i < PROF_COUNT; i++) {
        uint64_t v = ps3_prof.ticks[i];

        d[i] = v - last[i];
        last[i] = v;
        pm[i] = span != 0 ? (u32) (d[i] * 1000 / span) : 0;
    }
    other = d[PROF_DRAW_TEV] + d[PROF_DRAW_XF] + d[PROF_DRAW_STATE] + d[PROF_DRAW_VTX] + d[PROF_DRAW_CONST] +
            d[PROF_DRAW_TEX];
    other = d[PROF_DRAW] > other ? d[PROF_DRAW] - other : 0;
    if (last_now != 0) {
        // Per mille of the second; RSX waits partly fall inside the draw phases (ring fences, evictions).
        OSReport("heartbeat %u: GP per mille: draws %u (TEV %u, XF %u, state %u, vertices %u, constants %u, "
                 "textures %u, RSX commands %u), EFB copies %u, presents %u (flip waits %u); waits for data %u, "
                 "for RSX labels %u; main: asleep %u, ring full %u; per second: %u presents, %u draws, %u with no "
                 "register write since the last, %u batched, %u RSX draws, %u with the last draw's state, %u XF "
                 "reloads, %u VP loads, %u texture uploads of %u KB\n",
                 n, pm[PROF_DRAW], pm[PROF_DRAW_TEV], pm[PROF_DRAW_XF], pm[PROF_DRAW_STATE], pm[PROF_DRAW_VTX],
                 pm[PROF_DRAW_CONST], pm[PROF_DRAW_TEX], (u32) (span != 0 ? other * 1000 / span : 0),
                 pm[PROF_COPY], pm[PROF_PRESENT], pm[PROF_GP_FLIP], pm[PROF_GP_IDLE], pm[PROF_GP_RSX],
                 pm[PROF_MAIN_SLEEP], pm[PROF_MAIN_RING], rs->presents - last_presents, gs->draws - last_draws,
                 ps3_prof.batchable - last_batchable, ps3_prof.batched - last_batched, rs->rsx_draws - last_rsx,
                 ps3_prof.state_reuses - last_reuses,
                 ps3_prof.xf_reloads - last_reloads, rs->vp_loads - last_vp,
                 rs->texture_uploads - last_uploads, (u32) ((ps3_prof.upload_bytes - last_bytes) >> 10));
        // OSReport keeps 511 characters: the caches get their own line.
        OSReport("heartbeat %u: per second: display lists: %u drawn from the vertex cache, %u converted, %u run as "
                 "commands, %u evicted; textures: %u hashes, %u evicted, %u waits, %u not found; %u indirect TEVs; "
                 "TEV setups %u, %u built; vertex jobs: %u on the SPU, %u on the PPE, %u waits (%u per mille)\n",
                 n, ps3_prof.list_hits - last_hits, ps3_prof.list_builds - last_builds, ps3_prof.list_runs - last_runs,
                 ps3_prof.list_evictions - last_evictions, ps3_prof.tex_hashes - last_tex_hashes,
                 ps3_prof.tex_evictions - last_tex_evictions, ps3_prof.tex_waits - last_tex_waits,
                 ps3_prof.tex_failures - last_tex_failures, ps3_prof.indirect_tevs - last_indirect,
                 ps3_prof.tev_setups - last_tev_setups, ps3_prof.tev_builds - last_tev_builds,
                 ps3_prof.spu_jobs - last_spu_jobs, ps3_prof.spu_ppe - last_spu_ppe,
                 ps3_prof.spu_waits - last_spu_waits,
                 (u32) (span != 0 ? (ps3_prof.spu_wait_ticks - last_spu_ticks) * 1000 / span : 0));
    }
    last_tev_setups = ps3_prof.tev_setups;
    last_tev_builds = ps3_prof.tev_builds;
    last_spu_jobs = ps3_prof.spu_jobs;
    last_spu_ppe = ps3_prof.spu_ppe;
    last_spu_waits = ps3_prof.spu_waits;
    last_spu_ticks = ps3_prof.spu_wait_ticks;
    last_tex_hashes = ps3_prof.tex_hashes;
    last_tex_evictions = ps3_prof.tex_evictions;
    last_tex_waits = ps3_prof.tex_waits;
    last_tex_failures = ps3_prof.tex_failures;
    last_indirect = ps3_prof.indirect_tevs;
    last_hits = ps3_prof.list_hits;
    last_builds = ps3_prof.list_builds;
    last_runs = ps3_prof.list_runs;
    last_evictions = ps3_prof.list_evictions;
    last_now = now;
    last_draws = gs->draws;
    last_batchable = ps3_prof.batchable;
    last_batched = ps3_prof.batched;
    last_rsx = rs->rsx_draws;
    last_reuses = ps3_prof.state_reuses;
    last_reloads = ps3_prof.xf_reloads;
    last_vp = rs->vp_loads;
    last_uploads = rs->texture_uploads;
    last_presents = rs->presents;
    last_bytes = ps3_prof.upload_bytes;
}

// sdk_audio.c and synth.c.
void ps3_audio_stats(u32* frames, u32* ticks, u32* silent_ticks, u32* bad_commands);
void ps3_synth_stream_state(u32 out[10]);

// The audio program's command lists and AI periods a second, and the game's music stream: where it stops.
static void log_audio(u32 n)
{
    static u32 last_frames, last_ticks, last_silent;
    u32 frames, ticks, silent, bad, st[10];

    ps3_audio_stats(&frames, &ticks, &silent, &bad);
    ps3_synth_stream_state(st);
    OSReport("heartbeat %u: audio: %u command lists, %u AI periods, %u silent a second, %u bad lists; stream: "
             "load busy %u, blocks %u %u %u %u, voice flags %#x state %u at %08x, end %08x, loop %08x\n",
             n, frames - last_frames, ticks - last_ticks, silent - last_silent, bad, st[0], st[1], st[2], st[3], st[4],
             st[5], st[6], st[7], st[8], st[9]);
    last_frames = frames;
    last_ticks = ticks;
    last_silent = silent;
}

// The main thread's frame phases a second (mainprof.c): logic and render time per frame in µs, and every 10 s the
// sampler's busiest callbacks.
static void log_main(u32 n)
{
    static uint64_t last_now, last_logic, last_render, last_xfb, last_pad, last_gp_wait;
    static u32 last_frames, last_passes, last_parses, last_hits, last_misses, last_uncached;
    uint64_t now = ps3_prof_now(), span = now - last_now;
    uint64_t logic = ps3_prof.ticks[PROF_MAIN_LOGIC] - last_logic;
    uint64_t render = ps3_prof.ticks[PROF_MAIN_RENDER] - last_render;
    uint64_t xfb = ps3_prof.ticks[PROF_MAIN_XFB] - last_xfb, pad = ps3_prof.ticks[PROF_MAIN_PAD] - last_pad;
    uint64_t gp_wait_now = ps3_prof.ticks[PROF_GP_IDLE] + ps3_prof.ticks[PROF_GP_RSX] + ps3_prof.ticks[PROF_GP_FLIP];
    uint64_t gp_wait = gp_wait_now - last_gp_wait;
    uint64_t logic_max = __atomic_exchange_n(&ps3_prof.logic_max, 0, __ATOMIC_RELAXED);
    uint64_t render_max = __atomic_exchange_n(&ps3_prof.render_max, 0, __ATOMIC_RELAXED);
    u32 frames = ps3_prof.logic_frames - last_frames, passes = ps3_prof.render_passes - last_passes;
    // The host's mftb under qemu runs at another rate: its µs are not real, its shares are.
    u32 tb_mhz = (u32) (ps3_sys_timebase_freq() / 1000000);

    if (last_now != 0 && span != 0 && tb_mhz != 0) {
        OSReport("heartbeat %u: main per mille: logic %u, render %u, XFB %u, pad wait %u; %u logic frames, %u render "
                 "passes; per frame: logic %u us (longest %u), render %u us (longest %u); GP busy %u per mille\n",
                 n, (u32) (logic * 1000 / span), (u32) (render * 1000 / span), (u32) (xfb * 1000 / span),
                 (u32) (pad * 1000 / span), frames, passes, frames != 0 ? (u32) (logic / tb_mhz / frames) : 0,
                 (u32) (logic_max / tb_mhz), passes != 0 ? (u32) (render / tb_mhz / passes) : 0,
                 (u32) (render_max / tb_mhz),
                 gp_wait < span ? (u32) (1000 - gp_wait * 1000 / span) : 0);
        OSReport("heartbeat %u: dat: %u parses a second; motions: %u from the cache, %u added, %u parsed in place; "
                 "cache %u entries, %u KB\n",
                 n, ps3_prof.dat_parses - last_parses, ps3_prof.motion_hits - last_hits,
                 ps3_prof.motion_misses - last_misses, ps3_prof.motion_uncached - last_uncached,
                 ps3_prof.motion_entries, ps3_prof.motion_bytes >> 10);
    }
    if (n % 10 == 0) {
        ps3_mainprof_log(n);
    }
    last_now = now;
    last_logic = ps3_prof.ticks[PROF_MAIN_LOGIC];
    last_render = ps3_prof.ticks[PROF_MAIN_RENDER];
    last_xfb = ps3_prof.ticks[PROF_MAIN_XFB];
    last_pad = ps3_prof.ticks[PROF_MAIN_PAD];
    last_gp_wait = gp_wait_now;
    last_frames = ps3_prof.logic_frames;
    last_passes = ps3_prof.render_passes;
    last_parses = ps3_prof.dat_parses;
    last_hits = ps3_prof.motion_hits;
    last_misses = ps3_prof.motion_misses;
    last_uncached = ps3_prof.motion_uncached;
}

// Once a second, apart from the interrupts: a log that stops means the process is gone, counts that stop
// name the part that is stuck (step 3 diagnostics).
static void heartbeat_main(void* arg)
{
    static PsMutex lock = PS_MUTEX_INIT;
    static PsCond cond = PS_COND_INIT;
    u32 n;

    ps_mutex_lock(&lock);
    for (n = 1;; n++) {
        static const char* const waits[] = { "nothing", "ring data", "a flip", "an RSX label" };
        const char* owner;
        char rsx[320];
        GpStats gs;
        GxRsxStats rs;

        ps_cond_timedwait(&cond, &lock, 1000000);
        owner = ps3_diag.intr_owner;
        gp_get_stats(&gs);
        gx_rsx_get_stats(&rs);
        OSReport("heartbeat %u: %u retraces; GP %u commands, %u draws; %u presents, %u dropped draws\n", n,
                 retrace_count, gs.commands, gs.draws, rs.presents, rs.dropped_draws);
        OSReport("heartbeat %u: main %u disables, %u retrace waits; %u alarms, %u pad reads, %u CARD calls; "
                 "DVD %u reads, %u callbacks; ARQ %u posts, %u callbacks\n",
                 n, ps3_diag.main_disables, ps3_diag.retrace_waits, ps3_diag.alarms, ps3_diag.pad_reads,
                 ps3_diag.card_calls, ps3_diag.dvd_reads, ps3_diag.dvd_callbacks, ps3_diag.arq_posts,
                 ps3_diag.arq_callbacks);
        dev_debug_state(rsx, sizeof(rsx));
        dev_debug_dump_stall();
        OSReport("heartbeat %u: %u vblanks; GP waits for %s; producer waits for ring space %u; "
                 "interrupt lock: %s, at %s; %s; %u pads, buttons %#x\n",
                 n, ps3_diag.vblanks, waits[ps3_diag.gp_wait & 3], ps3_diag.ring_space_wait,
                 owner != NULL ? owner : "free", ps3_diag.intr_site != NULL ? ps3_diag.intr_site : "no handler",
                 rsx, ps3_diag.pads, ps3_diag.pad0_buttons);
        log_profile(n, &gs, &rs);
        {
            const void* q = ps3_diag.main_sleep;
            const char* site = ps3_diag.gp_site;

            OSReport("heartbeat %u: GP at %s, opcode %#x, draw %u; main sleeps on %s %p\n", n,
                     site != NULL ? site : "start", ps3_diag.gp_op, ps3_diag.gp_draw,
                     q == NULL ? "nothing" : q == &retrace_queue ? "the retrace queue" : "the queue at", q);
        }
        log_audio(n);
        log_main(n);
        if (ps3_diag.main_calls != 0) {
            log_calltrace(n);
        }
    }
}

void VIInit(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    OSInitThreadQueue(&retrace_queue);
    // GXInit writes to the GP ring, so the GP starts here, before it.
    if (!gx_rsx_init()) {
        OSPanic(__FILE__, __LINE__, "the RSX did not start\n");
    }
    gp_init(gx_rsx_backend(), GP_RING_SIZE);
    ps3_intr_set_handler(PS3_INTR_VI, retrace);
    dev_set_vblank(raise_retrace);
    ps_thread_start(heartbeat_main, NULL, "heartbeat");
    ps3_mainprof_start();
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = pre_callback;

    pre_callback = cb;
    OSRestoreInterrupts(enabled);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = post_callback;

    post_callback = cb;
    OSRestoreInterrupts(enabled);
    return old;
}

void VIWaitForRetrace(void)
{
    ps3_diag.retrace_waits++;
    OSSleepThread(&retrace_queue);
}

// The RSX output mode is the XMB's; the backend scales the frame buffer to it.
void VIConfigure(GXRenderModeObj* rm) {}

void VIFlush(void)
{
    BOOL enabled = OSDisableInterrupts();

    flush_pending = true;
    OSRestoreInterrupts(enabled);
}

void VISetNextFrameBuffer(void* fb)
{
    BOOL enabled = OSDisableInterrupts();

    next_buffer = fb;
    OSRestoreInterrupts(enabled);
}

// Black keeps the last frame on the screen; no frame is shown before the game turns it off.
void VISetBlack(BOOL value)
{
    BOOL enabled = OSDisableInterrupts();

    black = value;
    OSRestoreInterrupts(enabled);
}

u32 VIGetRetraceCount(void)
{
    return retrace_count;
}

u32 VIGetNextField(void)
{
    return 0;
}

u32 VIGetTvFormat(void)
{
    return VI_NTSC;
}

// No component cable: the game does not offer progressive scan.
u32 VIGetDTVStatus(void)
{
    return 0;
}
