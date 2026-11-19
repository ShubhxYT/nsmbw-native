#pragma once
// Horizon has no getifaddrs. The port does not need LAN discovery on Switch, so this reports "no
// interfaces" and the runtime's discovery loop finds nothing, which is the correct behaviour here.
#include <sys/socket.h>

struct ifaddrs {
    struct ifaddrs* ifa_next;
    char* ifa_name;
    unsigned int ifa_flags;
    struct sockaddr* ifa_addr;
    struct sockaddr* ifa_netmask;
    struct sockaddr* ifa_dstaddr;
    void* ifa_data;
    struct sockaddr* ifa_broadaddr;  // alias of ifa_dstaddr on Linux
};

#ifndef IFF_UP
#define IFF_UP 0x1
#endif
#ifndef IFF_LOOPBACK
#define IFF_LOOPBACK 0x8
#endif

static inline int getifaddrs(struct ifaddrs** out) {
    if (out) {
        *out = 0;
    }
    return -1;
}

static inline void freeifaddrs(struct ifaddrs* list) {
    (void)list;
}
