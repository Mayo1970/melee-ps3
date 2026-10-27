// The OS and VI calls the GX library makes that no port layer has yet, for the GX test programs.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "../os/udplog.h"

void OSReport(char* fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ps3_log("%s", buf);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    ps3_log("OSPanic %s:%d: %s\n", file, line, buf);
    abort();
}

u32 VIGetTvFormat(void)
{
    return VI_NTSC;
}

// GX calls these only on FIFO overflow and in GXAbortFrame.
OSThread* OSGetCurrentThread(void)
{
    return NULL;
}

s32 OSResumeThread(OSThread* thread)
{
    abort();
}

s32 OSSuspendThread(OSThread* thread)
{
    abort();
}

OSTime OSGetTime(void)
{
    abort();
}
