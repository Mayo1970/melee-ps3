// The netplay's UDP socket over PSL1GHT (link.h).
#include "link.h"

#include <arpa/inet.h>
#include <net/net.h>
#include <net/netctl.h>
#include <net/netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sysmodule/sysmodule.h>

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

// The UDP log has loaded the net module and started the library (udplog_cell.c).
bool ps3_link_open(uint16_t port)
{
    struct sockaddr_in s;
    Ps3NetAddr any = { 0, port };
    struct timeval tv = { 0, 10000 };
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
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    to_sockaddr(&any, &s);
    if (bind(sock, (struct sockaddr*) &s, sizeof(s)) < 0) {
        shutdown(sock, SHUT_RDWR);
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

bool ps3_link_resolve(const char* s, Ps3NetAddr* out)
{
    char host[128];
    const char* colon = strrchr(s, ':');
    struct hostent* h;
    size_t n;

    if (colon == NULL || (n = (size_t) (colon - s)) == 0 || n >= sizeof(host)) {
        return false;
    }
    memcpy(host, s, n);
    host[n] = '\0';
    out->port = (uint16_t) atoi(colon + 1);
    if (inet_addr(host) != INADDR_NONE) {
        out->ip = ntohl(inet_addr(host));
        return out->port != 0;
    }
    h = gethostbyname(host);
    if (h == NULL || h->h_addr_list[0] == NULL) {
        return false;
    }
    out->ip = ntohl(((struct in_addr*) h->h_addr_list[0])->s_addr);
    return out->port != 0;
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
