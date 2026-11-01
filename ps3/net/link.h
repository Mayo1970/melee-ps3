// The netplay's UDP socket (ps3/net/netplay.c): link_cell.c over PSL1GHT, tests/link_host.c on Linux. Standard C
// types only, so SDK sources can include it. Addresses and ports are in host byte order.
#ifndef PS3_NET_LINK_H
#define PS3_NET_LINK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Ps3NetAddr {
    uint32_t ip;
    uint16_t port;
} Ps3NetAddr;

// Opens the socket on port with broadcasts allowed; the host build may take another port from its environment.
bool ps3_link_open(uint16_t port);
bool ps3_link_send(const Ps3NetAddr* to, const void* buf, int len);
// Waits up to about usec for a datagram: its length, or 0 when none came.
int ps3_link_recv(Ps3NetAddr* from, void* buf, int cap, unsigned usec);
// "a.b.c.d:port" or "name:port"; false when it does not resolve.
bool ps3_link_resolve(const char* s, Ps3NetAddr* out);
// This console's address on its own network, with the socket's port: the matchmaker sees only the router's.
bool ps3_link_local(Ps3NetAddr* out);

#endif
