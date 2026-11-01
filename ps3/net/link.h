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
// This console's address on its own network, with the socket's port.
bool ps3_link_local(Ps3NetAddr* out);
// UPnP (upnp.c), from a socket of their own. A datagram to to, then the first answer and its sender (*from); a TCP
// request to to, then all it answers until it closes. The answer's length, or -1 on an error or after ms.
int ps3_link_udp_ask(const Ps3NetAddr* to, const void* req, int len, void* resp, int cap, unsigned ms,
                     Ps3NetAddr* from);
int ps3_link_tcp_ask(const Ps3NetAddr* to, const void* req, int len, void* resp, int cap, unsigned ms);

#endif
