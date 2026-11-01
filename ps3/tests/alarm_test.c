// Host test of ps3_intr_wait_alarms with the OS alarms: the main thread waits for a 60 Hz alarm to fill a queue,
// as gm_801A4D34 waits for the pad alarm, and must take every alarm in time without spinning.
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include <dolphin/os.h>
#include <ps3_intr.h>

#include "../os/intr.h"

#define FRAMES 180

unsigned long long ps3_sys_timebase(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000000000ull + (unsigned long long) ts.tv_nsec;
}

unsigned long long ps3_sys_timebase_freq(void)
{
    return 1000000000ull;
}

long long ps3_sys_local_time(void)
{
    return (long long) time(NULL);
}

void OSReport(char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static OSAlarm alarm;
static int queued;
static int fired;
static OSTime first_at;

// Runs on the alarm thread with interrupts disabled, as the pad alarm does.
static void on_alarm(OSAlarm* a, OSContext* context)
{
    if (queued++ == 0) {
        first_at = OSGetTime();
    }
    fired++;
}

// The game loop's wait, with interrupts disabled; returns how often it slept.
static int wait_queue(unsigned long long usec, int* count)
{
    int sleeps = 0;

    while ((*count = queued) == 0) {
        sleeps++;
        ps3_intr_wait_alarms(usec);
    }
    queued = 0;
    return sleeps;
}

int main(void)
{
    OSTime period = OSSecondsToTicks(1) / 60;
    OSTime worst = 0;
    OSTime start;
    int frames = 0;
    int sleeps = 0;
    int late_frames = 0;
    int failures = 0;
    int i;

    ps3_intr_init();
    OSInitAlarm();
    OSCreateAlarm(&alarm);
    OSSetPeriodicAlarm(&alarm, OSGetTime() + period, period, (OSAlarmHandler) on_alarm);
    // A timeout far above the period: only the alarm thread's wakeup keeps the frames coming.
    while (frames < FRAMES) {
        int count;
        OSTime late;
        BOOL enabled;

        enabled = OSDisableInterrupts();
        sleeps += wait_queue(500000, &count);
        late = OSGetTime() - first_at;
        OSRestoreInterrupts(enabled);
        if (late > worst) {
            worst = late;
        }
        late_frames += late > period;
        frames += count;
    }
    OSCancelAlarm(&alarm);
    frames += queued;
    printf("woken: %d frames of %d alarms, %d sleeps, %d a period or more late, latest %lld us after its alarm\n",
           frames, fired, sleeps, late_frames, (long long) OSTicksToMicroseconds(worst));
    // Without the alarm thread's wakeup each wait would last its 500 ms. A busy host can delay a wakeup by a period.
    if (frames != fired || worst > 4 * period || sleeps > 2 * FRAMES) {
        printf("FAIL\n");
        failures++;
    }

    // Nothing armed: each wait ends at its timeout.
    start = OSGetTime();
    for (i = 0; i < 100; i++) {
        BOOL enabled = OSDisableInterrupts();

        ps3_intr_wait_alarms(1000);
        OSRestoreInterrupts(enabled);
    }
    {
        long long each = (long long) OSTicksToMicroseconds(OSGetTime() - start) / 100;

        printf("timed out: %lld us a wait for 1000\n", each);
        if (each < 1000 || each > 20000) {
            printf("FAIL\n");
            failures++;
        }
    }
    printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures != 0;
}
