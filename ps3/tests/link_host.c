// The netplay's UDP socket on the host game build (../net/link.h). MELEE_HOST_NET_PORT binds another port, so two
// instances share a machine; MELEE_HOST_NET_LAG="ms[:jitter_ms[:loss_percent]]" holds back or drops what arrives.
#include "../net/link.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HELD 256
#define MAX_DGRAM 1500

typedef struct Held {
    uint64_t due;
    Ps3NetAddr from;
    int len;
    unsigned char data[MAX_DGRAM];
} Held;

static int sock = -1;
static uint16_t bound;
static unsigned lag_us, jitter_us, loss;
static Held held[HELD];
static int nheld;
static unsigned seed = 1;

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
}

static void to_sockaddr(const Ps3NetAddr* a, struct sockaddr_in* s)
{
    memset(s, 0, sizeof(*s));
    s->sin_family = AF_INET;
    s->sin_port = htons(a->port);
    s->sin_addr.s_addr = htonl(a->ip);
}

bool ps3_link_open(uint16_t port)
{
    struct sockaddr_in s;
    Ps3NetAddr any = { 0, port };
    const char* env = getenv("MELEE_HOST_NET_PORT");
    const char* lag = getenv("MELEE_HOST_NET_LAG");
    int on = 1;

    if (sock >= 0) {
        return true;
    }
    if (env != NULL) {
        any.port = (uint16_t) atoi(env);
    }
    if (lag != NULL) {
        unsigned ms = 0, jitter = 0;

        sscanf(lag, "%u:%u:%u", &ms, &jitter, &loss);
        lag_us = ms * 1000u;
        jitter_us = jitter * 1000u;
    }
    seed = (unsigned) now_us() ^ any.port;
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return false;
    }
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    to_sockaddr(&any, &s);
    if (bind(sock, (struct sockaddr*) &s, sizeof(s)) < 0) {
        close(sock);
        sock = -1;
        return false;
    }
    bound = any.port;
    printf("link_host: port %u, lag %u ms, jitter %u ms, loss %u%%\n", bound, lag_us / 1000, jitter_us / 1000, loss);
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

// Takes everything waiting on the socket into the held list, each due after the lag and its jitter.
static void drain(void)
{
    struct sockaddr_in s;
    socklen_t slen = sizeof(s);
    unsigned char buf[MAX_DGRAM];
    int n;

    while ((n = (int) recvfrom(sock, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*) &s, &slen)) > 0) {
        Held* h;

        slen = sizeof(s);
        if (nheld == HELD || (loss != 0 && (unsigned) rand_r(&seed) % 100u < loss)) {
            continue;
        }
        h = &held[nheld++];
        h->due = now_us() + lag_us + (jitter_us != 0 ? (unsigned) rand_r(&seed) % jitter_us : 0);
        h->from.ip = ntohl(s.sin_addr.s_addr);
        h->from.port = ntohs(s.sin_port);
        h->len = n;
        memcpy(h->data, buf, (size_t) n);
    }
}

int ps3_link_recv(Ps3NetAddr* from, void* buf, int cap, unsigned usec)
{
    uint64_t end = now_us() + usec;

    if (sock < 0) {
        return 0;
    }
    for (;;) {
        uint64_t t = now_us(), next = end;
        struct pollfd p = { sock, POLLIN, 0 };
        int i, first = -1;

        drain();
        for (i = 0; i < nheld; i++) {
            if (first < 0 || held[i].due < held[first].due) {
                first = i;
            }
        }
        if (first >= 0 && held[first].due <= t) {
            int n = held[first].len < cap ? held[first].len : cap;

            *from = held[first].from;
            memcpy(buf, held[first].data, (size_t) n);
            held[first] = held[--nheld];
            return n;
        }
        if (t >= end) {
            return 0;
        }
        if (first >= 0 && held[first].due < next) {
            next = held[first].due;
        }
        poll(&p, 1, (int) ((next - t + 999) / 1000));
    }
}

// The route's source address towards a public one; connecting a UDP socket sends nothing.
bool ps3_link_local(Ps3NetAddr* out)
{
    struct sockaddr_in s;
    Ps3NetAddr probe = { 0x08080808u, 53 };
    socklen_t len = sizeof(s);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    bool ok;

    if (fd < 0) {
        return false;
    }
    to_sockaddr(&probe, &s);
    ok = connect(fd, (struct sockaddr*) &s, sizeof(s)) == 0 && getsockname(fd, (struct sockaddr*) &s, &len) == 0;
    close(fd);
    if (!ok) {
        return false;
    }
    out->ip = ntohl(s.sin_addr.s_addr);
    out->port = bound;
    return true;
}

static void set_timeout(int fd, unsigned ms)
{
    struct timeval tv = { (time_t) (ms / 1000), (suseconds_t) (ms % 1000) * 1000 };

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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
        n = (int) recvfrom(fd, resp, (size_t) cap, 0, (struct sockaddr*) &s, &slen);
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
    while (n < cap && (got = (int) recv(fd, (char*) resp + n, (size_t) (cap - n), 0)) > 0) {
        n += got;
    }
    close(fd);
    return n > 0 ? n : -1;
}
