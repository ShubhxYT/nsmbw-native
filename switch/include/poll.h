#pragma once
// poll() for Dawn's SystemEvent. libnx's BSD layer supplies pollfd and poll(), but newlib has no
// <poll.h>, so this declares the small subset Dawn uses.

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int fd;
    short events;
    short revents;
};

typedef unsigned long nfds_t;

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#ifdef __cplusplus
}
#endif
