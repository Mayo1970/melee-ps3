// The lv2 side of the OS layer through PSL1GHT. See sys.h.
#include "sys.h"
#include "udplog.h"

#include <stdlib.h>

#include <ppu-asm.h>
#include <sys/memory.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sysutil/sysutil.h>

// The game's main thread, below the port's threads (1001): its loop spins between frames for the pad alarm.
// 1 MB of stack, as HSD walks object trees recursively.
SYS_PROCESS_PARAM(1100, 0x100000);

static volatile bool quit;
static volatile bool overlay;

static void on_sysutil(u64 status, u64 param, void* user)
{
    if (status == SYSUTIL_EXIT_GAME) {
        quit = true;
    } else if (status == SYSUTIL_DRAW_BEGIN) {
        overlay = true;
    } else if (status == SYSUTIL_DRAW_END) {
        overlay = false;
    }
    // XMB opened (0x131) and closed (0x132), its drawing begins (0x121) and ends (0x122).
    ps3_log("sys: XMB event %#llx\n", (unsigned long long) status);
}

void ps3_sys_init(void)
{
    ps3_log_init();
    ps3_log("melee: boot, %u MB free\n", ps3_sys_free_mb());
    sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, on_sysutil, NULL);
}

unsigned long long ps3_sys_timebase(void)
{
    return __gettime();
}

unsigned long long ps3_sys_timebase_freq(void)
{
    return sysGetTimebaseFrequency();
}

long long ps3_sys_local_time(void)
{
    u64 sec = 0, nsec = 0;
    s32 zone = 0, summer = 0;

    sysGetCurrentTime(&sec, &nsec);
    // Minutes east of UTC, and whether summer time adds an hour.
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &zone);
    sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &summer);
    return (long long) sec + ((long long) zone + (summer ? 60 : 0)) * 60;
}

// Container creation fails cleanly when the size does not fit; an over-budget malloc can hang lv2.
unsigned ps3_sys_free_mb(void)
{
    unsigned lo = 1, hi = 256, best = 0;

    while (lo <= hi) {
        unsigned mid = (lo + hi) / 2;
        sys_mem_container_t c;

        if (sysMemContainerCreate(&c, (size_t) mid << 20) == 0) {
            sysMemContainerDestroy(c);
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

bool ps3_sys_poll(void)
{
    sysUtilCheckCallback();
    return quit;
}

bool ps3_sys_overlay(void)
{
    return overlay;
}

void ps3_sys_exit(void)
{
    ps3_log("melee: exit\n");
    sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
    // Where the exit path stops, if the console freezes on quit (step 3 diagnostics).
    ps3_log("melee: exit: XMB callback removed, calling exit()\n");
    exit(0);
}
