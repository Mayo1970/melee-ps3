// The GameCube interrupt model on PS3 threads. Disabling interrupts takes one lock, and handlers run
// only while holding it, so a handler never runs inside a disabled section of any thread.
#include "intr.h"
#include "diag.h"
#include "prof.h"
#include "sync.h"

#include <stdbool.h>

#include <dolphin/os.h>
#include <ps3_intr.h>

Ps3Diag ps3_diag;
Ps3Prof ps3_prof;

static PsMutex intr_lock = PS_MUTEX_INIT;
// Its address differs per thread; ps3_intr_init keeps the main thread's.
static __thread int thread_marker;
static int* main_marker;
// Per thread, as MSR[EE] was saved and restored per thread on the GameCube.
static __thread bool disabled;

static PsMutex wake_lock = PS_MUTEX_INIT;
static PsCond wake_cond = PS_COND_INIT;
static unsigned raised;
static Ps3IntrHandler handlers[PS3_INTR_COUNT];
static const char* const source_names[PS3_INTR_COUNT] = { "DVD", "ARAM", "DSP", "AI", "EXI", "PE token",
                                                                  "PE finish", "VI" };

static unsigned take_raised(void)
{
    unsigned bits;

    ps_mutex_lock(&wake_lock);
    bits = raised;
    raised = 0;
    ps_mutex_unlock(&wake_lock);
    return bits;
}

// Runs raised handlers until none are left. The caller holds intr_lock.
static void run_raised(void)
{
    unsigned bits;

    while ((bits = take_raised()) != 0) {
        int i;
        for (i = 0; i < PS3_INTR_COUNT; i++) {
            if ((bits & (1u << i)) && handlers[i] != NULL) {
                ps3_diag.intr_site = source_names[i];
                if (ps3_diag.trace) {
                    OSReport("intr: %s\n", source_names[i]);
                }
                handlers[i]();
                ps3_diag.intr_site = NULL;
                // A handler may enable interrupts; the rest still run disabled.
                OSDisableInterrupts();
            }
        }
    }
}

BOOL OSDisableInterrupts(void)
{
    if (&thread_marker == main_marker) {
        ps3_diag.main_disables++;
    }
    if (disabled) {
        return FALSE;
    }
    ps_mutex_lock(&intr_lock);
    disabled = true;
    ps3_diag.intr_owner = &thread_marker == main_marker ? "main" : ps_thread_name() ? ps_thread_name() : "other";
    return TRUE;
}

BOOL OSEnableInterrupts(void)
{
    if (!disabled) {
        return TRUE;
    }
    // A pending interrupt is taken as interrupts come on, as on the GameCube.
    if (__atomic_load_n(&raised, __ATOMIC_ACQUIRE) != 0) {
        run_raised();
    }
    disabled = false;
    ps3_diag.intr_owner = NULL;
    ps_mutex_unlock(&intr_lock);
    return FALSE;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    return level ? OSEnableInterrupts() : OSDisableInterrupts();
}

void ps3_intr_set_handler(int source, Ps3IntrHandler handler)
{
    handlers[source] = handler;
}

void ps3_intr_raise(int source)
{
    ps_mutex_lock(&wake_lock);
    raised |= 1u << source;
    ps_cond_signal(&wake_cond);
    ps_mutex_unlock(&wake_lock);
}

static void intr_main(void* arg)
{
    for (;;) {
        ps_mutex_lock(&wake_lock);
        while (raised == 0) {
            ps_cond_wait(&wake_cond, &wake_lock);
        }
        ps_mutex_unlock(&wake_lock);
        OSDisableInterrupts();
        run_raised();
        OSEnableInterrupts();
    }
}

// The SDK's own handlers, by GameCube interrupt number. Only the PE ones are raised; the mask is not
// modelled, as GX unmasks them at init and nothing masks them later.
static __OSInterruptHandler os_handlers[__OS_INTERRUPT_MAX];
static OSInterruptMask os_mask = ~0u;

static void run_os_handler(__OSInterrupt interrupt)
{
    if (os_handlers[interrupt] != NULL) {
        os_handlers[interrupt](interrupt, NULL);
    }
}

static void pe_token(void)
{
    run_os_handler(__OS_INTERRUPT_PI_PE_TOKEN);
}

static void pe_finish(void)
{
    run_os_handler(__OS_INTERRUPT_PI_PE_FINISH);
}

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler)
{
    __OSInterruptHandler old = os_handlers[interrupt];

    os_handlers[interrupt] = handler;
    if (interrupt == __OS_INTERRUPT_PI_PE_TOKEN) {
        ps3_intr_set_handler(PS3_INTR_PE_TOKEN, pe_token);
    } else if (interrupt == __OS_INTERRUPT_PI_PE_FINISH) {
        ps3_intr_set_handler(PS3_INTR_PE_FINISH, pe_finish);
    }
    return old;
}

__OSInterruptHandler __OSGetInterruptHandler(__OSInterrupt interrupt)
{
    return os_handlers[interrupt];
}

OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = os_mask;

    os_mask |= mask;
    return old;
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask)
{
    OSInterruptMask old = os_mask;

    os_mask &= ~mask;
    return old;
}

// Handlers switch FP contexts on the GameCube; PS3 threads keep their own.
void OSClearContext(OSContext* context) {}

void OSSetCurrentContext(OSContext* context) {}

// Thread queues until the OS layer has threads: a sleeper waits, with intr_lock released, until its queue
// is woken. Interrupts therefore run while it sleeps, as on the GameCube.
#define MAX_QUEUES 64

static PsCond sleep_cond = PS_COND_INIT;
static struct {
    OSThreadQueue* queue;
    u32 wakeups;
} queues[MAX_QUEUES];

// The caller holds intr_lock.
static u32* queue_wakeups(OSThreadQueue* queue)
{
    int i;

    for (i = 0; i < MAX_QUEUES; i++) {
        if (queues[i].queue == queue) {
            return &queues[i].wakeups;
        }
    }
    for (i = 0; i < MAX_QUEUES; i++) {
        if (queues[i].queue == NULL) {
            queues[i].queue = queue;
            return &queues[i].wakeups;
        }
    }
    OSReport("OSSleepThread: more than %d thread queues\n", MAX_QUEUES);
    return &queues[0].wakeups;
}

void OSInitThreadQueue(OSThreadQueue* queue)
{
    queue->head = NULL;
    queue->tail = NULL;
}

void OSSleepThread(OSThreadQueue* queue)
{
    BOOL level = OSDisableInterrupts();
    u32* wakeups = queue_wakeups(queue);
    u32 seen = *wakeups;
    uint64_t t = ps3_prof_now();

    if (&thread_marker == main_marker) {
        ps3_diag.main_sleep = queue;
    }
    while (*wakeups == seen) {
        const char* owner = ps3_diag.intr_owner;

        // The wait gives the lock up.
        ps3_diag.intr_owner = NULL;
        ps_cond_wait(&sleep_cond, &intr_lock);
        ps3_diag.intr_owner = owner;
    }
    if (&thread_marker == main_marker) {
        ps3_prof_lap(PROF_MAIN_SLEEP, &t);
        ps3_diag.main_sleep = NULL;
    }
    OSRestoreInterrupts(level);
}

void OSWakeupThread(OSThreadQueue* queue)
{
    BOOL level = OSDisableInterrupts();

    (*queue_wakeups(queue))++;
    ps_cond_broadcast(&sleep_cond);
    OSRestoreInterrupts(level);
}

// Alarm handlers run under intr_lock, so one cannot run between the caller's check and the wait.
static PsCond alarm_cond = PS_COND_INIT;
static int alarm_waiters;

void ps3_intr_wait_alarms(unsigned long long usec)
{
    BOOL level = OSDisableInterrupts();
    const char* owner = ps3_diag.intr_owner;
    uint64_t t = ps3_prof_now();

    if (&thread_marker == main_marker) {
        ps3_diag.main_sleep = &alarm_cond;
    }
    alarm_waiters++;
    ps3_diag.intr_owner = NULL;
    ps_cond_timedwait(&alarm_cond, &intr_lock, usec);
    ps3_diag.intr_owner = owner;
    alarm_waiters--;
    if (&thread_marker == main_marker) {
        ps3_prof_lap(PROF_MAIN_PAD, &t);
        ps3_diag.main_sleep = NULL;
    }
    OSRestoreInterrupts(level);
}

void ps3_intr_alarms_ran(void)
{
    if (alarm_waiters != 0) {
        ps_cond_broadcast(&alarm_cond);
    }
}

void ps3_intr_init(void)
{
    static bool started;

    if (!started) {
        started = true;
        main_marker = &thread_marker;
        if (ps3_calltrace_main != NULL) {
            ps3_calltrace_main();
        }
        ps_thread_start(intr_main, NULL, "interrupts");
    }
}
