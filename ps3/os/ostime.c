// The GameCube time base (OS_TIMER_CLOCK ticks since 2000-01-01, local time) from the PPE time base.
#include "ostime.h"

#include <time.h>

#include <dolphin/os.h>

#include "sys.h"

#define SECONDS_1970_TO_2000 946684800

static u64 tb_freq;
static u64 tb_start;
static OSTime time_start;

void ps3_time_init(void)
{
    tb_freq = ps3_sys_timebase_freq();
    tb_start = ps3_sys_timebase();
    time_start = (OSTime) (ps3_sys_local_time() - SECONDS_1970_TO_2000) * OS_TIMER_CLOCK;
}

OSTime OSGetTime(void)
{
    u64 d;

    if (tb_freq == 0) {
        ps3_time_init();
    }
    d = ps3_sys_timebase() - tb_start;
    // Whole seconds first, so the products stay within 64 bits.
    return time_start + (OSTime) (d / tb_freq * OS_TIMER_CLOCK + d % tb_freq * OS_TIMER_CLOCK / tb_freq);
}

OSTick OSGetTick(void)
{
    return (OSTick) OSGetTime();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
    OSTime secs = ticks / OS_TIMER_CLOCK;
    OSTime rem = ticks % OS_TIMER_CLOCK;
    time_t t;
    struct tm tm;

    if (rem < 0) {
        rem += OS_TIMER_CLOCK;
        secs--;
    }
    t = (time_t) (secs + SECONDS_1970_TO_2000);
    gmtime_r(&t, &tm);
    td->sec = tm.tm_sec;
    td->min = tm.tm_min;
    td->hour = tm.tm_hour;
    td->mday = tm.tm_mday;
    td->mon = tm.tm_mon;
    td->year = tm.tm_year + 1900;
    td->wday = tm.tm_wday;
    td->yday = tm.tm_yday;
    td->msec = (int) (rem / (OS_TIMER_CLOCK / 1000));
    td->usec = (int) (rem / (OS_TIMER_CLOCK / 1000000) % 1000);
}
