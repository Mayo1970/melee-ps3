// UDP log to the PC through PSL1GHT's net library. See udplog.h.
#include "udplog.h"
#include "sync.h"

#include <arpa/inet.h>
#include <net/net.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sysmodule/sysmodule.h>

#ifndef PS3_LOG_IP
#define PS3_LOG_IP "192.168.178.99"
#endif
#define PS3_LOG_PORT 18194

static int sock = -1;
static struct sockaddr_in addr;
static PsMutex lock = PS_MUTEX_INIT;

void ps3_log_init(void)
{
    sysModuleLoad(SYSMODULE_NET);
    netInitialize();
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&addr, 0, sizeof(addr));
    // PSL1GHT has a BSD sin_len; sendto fails with ENOENT when it stays 0.
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PS3_LOG_PORT);
    addr.sin_addr.s_addr = inet_addr(PS3_LOG_IP);
}

void ps3_log(const char* fmt, ...)
{
    static char buf[1024];
    va_list ap;
    int len;

    ps_mutex_lock(&lock);
    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > (int) sizeof(buf) - 1) {
        len = sizeof(buf) - 1;
    }
    if (sock >= 0 && len > 0) {
        sendto(sock, buf, (size_t) len, 0, (struct sockaddr*) &addr, sizeof(addr));
    }
    ps_mutex_unlock(&lock);
}
