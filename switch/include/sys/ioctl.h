#pragma once
// Horizon has no ioctl. sqlite3's unix VFS includes this header unconditionally, so the port provides
// the declaration; the calls it makes are all guarded by runtime checks that fail gracefully.
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#ifndef F_FULLFSYNC
#define F_FULLFSYNC 51
#endif
