// The SDK's VI calls on the PS3 display. Each vertical blank raises the retrace interrupt: the game's callbacks
// run around it, and a flushed frame buffer goes to the GP thread to be shown (the RSX belongs to that thread).
#include <stdbool.h>

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "../gx/backend.h"
#include "../gx/gp.h"
#include "../gx/rsxdev.h"
#include "../os/diag.h"
#include "../os/intr.h"
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
        ps3_sys_exit();
    }
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
        if (dev_debug_dump_stall()) {
            gx_rsx_debug_hang();
        }
        OSReport("heartbeat %u: %u vblanks; GP waits for %s; producer waits for ring space %u; "
                 "interrupt lock: %s, at %s; %s; %u pads, buttons %#x\n",
                 n, ps3_diag.vblanks, waits[ps3_diag.gp_wait & 3], ps3_diag.ring_space_wait,
                 owner != NULL ? owner : "free", ps3_diag.intr_site != NULL ? ps3_diag.intr_site : "no handler",
                 rsx, ps3_diag.pads, ps3_diag.pad0_buttons);
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
