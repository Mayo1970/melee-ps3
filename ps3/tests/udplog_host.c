// The UDP log on the host tests: to stdout.
#include "../os/udplog.h"

#include <stdarg.h>
#include <stdio.h>

void ps3_log_init(void) {}

void ps3_log(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
