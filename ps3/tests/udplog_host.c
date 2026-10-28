// The UDP log on the host tests: to stdout.
#include "../os/udplog.h"

#include <stdarg.h>
#include <stdio.h>

// Line by line, so a crash keeps the log up to it.
void ps3_log_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
}

void ps3_log(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
