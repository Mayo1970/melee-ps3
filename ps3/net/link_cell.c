// The netplay's UDP socket over PSL1GHT (link.h).
#include "link.h"

#include <arpa/inet.h>
#include <net/net.h>
#include <net/netctl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sysmodule/sysmodule.h>
#include <unistd.h>

static int sock = -1;
static uint16_t bound;

static void to_sockaddr(const Ps3NetAddr* a, struct sockaddr_in* s)
{
    memset(s, 0, sizeof(*s));
    // PSL1GHT has a BSD sin_len; sendto fails with ENOENT when it stays 0.
    s->sin_len = sizeof(*s);
    s->sin_family = AF_INET;
    s->sin_port = htons(a->port);
    s->sin_addr.s_addr = htonl(a->ip);
}

static void set_timeout(int fd, unsigned ms)
{
    struct timeval tv = { (long) (ms / 1000), (long) (ms % 1000) * 1000 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// The UDP log has loaded the net module and started the library (udplog_cell.c).
bool ps3_link_open(uint16_t port)
{
    struct sockaddr_in s;
    Ps3NetAddr any = { 0, port };
    int on = 1;

    if (sock >= 0) {
        return true;
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    // The receive thread wakes at least this often for its resends.
    set_timeout(sock, 10);
    to_sockaddr(&any, &s);
    if (bind(sock, (struct sockaddr*) &s, sizeof(s)) < 0) {
        close(sock);
        sock = -1;
        return false;
    }
    bound = port;
    return true;
}

bool ps3_link_send(const Ps3NetAddr* to, const void* buf, int len)
{
    struct sockaddr_in s;

    if (sock < 0) {
        return false;
    }
    to_sockaddr(to, &s);
    return sendto(sock, buf, (size_t) len, 0, (struct sockaddr*) &s, sizeof(s)) == len;
}

int ps3_link_recv(Ps3NetAddr* from, void* buf, int cap, unsigned usec)
{
    struct sockaddr_in s;
    socklen_t len = sizeof(s);
    int n;

    if (sock < 0) {
        return 0;
    }
    n = recvfrom(sock, buf, (size_t) cap, 0, (struct sockaddr*) &s, &len);
    if (n <= 0) {
        return 0;
    }
    from->ip = ntohl(s.sin_addr.s_addr);
    from->port = ntohs(s.sin_port);
    return n;
}

bool ps3_link_local(Ps3NetAddr* out)
{
    static bool loaded;
    union net_ctl_info info;

    if (!loaded) {
        loaded = true;
        sysModuleLoad(SYSMODULE_NETCTL);
        netCtlInit();
    }
    if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info) < 0 || inet_addr(info.ip_address) == INADDR_NONE) {
        return false;
    }
    out->ip = ntohl(inet_addr(info.ip_address));
    out->port = bound;
    return true;
}

int ps3_link_udp_ask(const Ps3NetAddr* to, const void* req, int len, void* resp, int cap, unsigned ms,
                     Ps3NetAddr* from)
{
    struct sockaddr_in s;
    socklen_t slen = sizeof(s);
    int fd = socket(AF_INET, SOCK_DGRAM, 0), n = -1;

    if (fd < 0) {
        return -1;
    }
    set_timeout(fd, ms);
    to_sockaddr(to, &s);
    if (sendto(fd, req, (size_t) len, 0, (struct sockaddr*) &s, sizeof(s)) == len) {
        n = recvfrom(fd, resp, (size_t) cap, 0, (struct sockaddr*) &s, &slen);
        from->ip = ntohl(s.sin_addr.s_addr);
        from->port = ntohs(s.sin_port);
    }
    close(fd);
    return n > 0 ? n : -1;
}

int ps3_link_tcp_ask(const Ps3NetAddr* to, const void* req, int len, void* resp, int cap, unsigned ms)
{
    struct sockaddr_in s;
    int fd = socket(AF_INET, SOCK_STREAM, 0), n = 0, got;

    if (fd < 0) {
        return -1;
    }
    set_timeout(fd, ms);
    to_sockaddr(to, &s);
    if (connect(fd, (struct sockaddr*) &s, sizeof(s)) < 0 || send(fd, req, (size_t) len, 0) != len) {
        close(fd);
        return -1;
    }
    while (n < cap && (got = recv(fd, (char*) resp + n, (size_t) (cap - n), 0)) > 0) {
        n += got;
    }
    close(fd);
    return n > 0 ? n : -1;
}
