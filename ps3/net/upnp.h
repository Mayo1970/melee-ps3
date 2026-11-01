// UPnP on the router (upnp.c), for a hosting console: an open UDP port and the router's public address. Standard C
// types only. Blocking for a few seconds: the netplay thread calls it, outside its lock.
#ifndef PS3_NET_UPNP_H
#define PS3_NET_UPNP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Ps3Upnp {
    bool router;        // a router answered UPnP
    bool opened;        // the port leads to this console (our mapping, or one that was there)
    uint32_t public_ip; // the router's internet address, 0 when unknown
} Ps3Upnp;

// Opens UDP port to local_ip:port for lease_s seconds (0: until removed).
void ps3_upnp_open(uint16_t port, uint32_t local_ip, uint32_t lease_s, Ps3Upnp* out);
// Removes the mapping ps3_upnp_open added, if it added one.
void ps3_upnp_close(uint16_t port);

#endif
