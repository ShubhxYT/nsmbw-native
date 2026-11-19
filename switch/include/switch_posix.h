#pragma once
// POSIX pieces Horizon lacks, declared for the vendored libraries (Abseil, Dawn, protobuf helpers)
// that assume a desktop libc. This header is force-included into their builds; the definitions live
// in switch/switch_shims.cpp and switch/absl_horizon.cpp.
//
// Everything here is a no-op or a documented approximation: none of these are load-bearing for
// rendering or emulation, they exist so the code paths that call them compile and behave sanely.

// newlib only compiles the tm_gmtoff/tm_zone members of struct tm when these are defined before
// <time.h> is first included, which is why they come before every #include in this file. cctz (the
// time-zone code in Abseil) reads them by their BSD names.
#ifndef __TM_GMTOFF
#define __TM_GMTOFF __tm_gmtoff
#endif
#ifndef __TM_ZONE
#define __TM_ZONE __tm_zone
#endif
#ifndef tm_gmtoff
#define tm_gmtoff __tm_gmtoff
#endif
#ifndef tm_zone
#define tm_zone __tm_zone
#endif

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/types.h>

// ---------------------------------------------------------------------------------------------
// Anonymous mappings (Abseil's LowLevelAlloc)
// ---------------------------------------------------------------------------------------------

#ifndef PROT_READ
#define PROT_READ 0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 0x02
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif
#ifndef MAP_FAILED
#define MAP_FAILED (reinterpret_cast<void*>(-1))
#endif
#ifndef MADV_DONTDUMP
#define MADV_DONTDUMP 16
#endif

extern "C" {
void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
int mprotect(void* addr, size_t len, int prot);
int madvise(void* addr, size_t length, int advice);
long sysconf(int name);
int getpagesize(void);
}

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

// ---------------------------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------------------------

extern "C" {
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int putenv(char* string);
}

// newlib on Horizon exposes only the GNU _strerror_r; Abseil and Dawn call the POSIX one.
extern "C" int strerror_r(int errnum, char* buf, size_t buflen);
extern "C" int getpid(void);
extern "C" int sched_yield(void);
extern "C" int usleep(useconds_t microseconds);

// ---------------------------------------------------------------------------------------------
// Signals: Horizon has no POSIX signal masks. Abseil only uses these to keep its thread-identity
// allocation out of signal handlers; returning success is correct when no handler that allocates
// can run. The functions are named switch_* and reached through macros, because newlib's signal.h
// already owns the POSIX spellings on some releases.
// ---------------------------------------------------------------------------------------------

#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#endif
#ifndef SIG_UNBLOCK
#define SIG_UNBLOCK 1
#endif

extern "C" {
typedef struct switch_sigset {
    unsigned long bits[16];
} switch_sigset_t;
int switch_sigfillset(void* set);
int switch_sigemptyset(void* set);
int switch_sigaddset(void* set, int signum);
int switch_pthread_sigmask(int how, const void* set, void* oldset);
}

#define sigfillset(set) switch_sigfillset(set)
#define sigemptyset(set) switch_sigemptyset(set)
#define sigaddset(set, signum) switch_sigaddset(set, signum)
#define pthread_sigmask(how, set, oldset) switch_pthread_sigmask(how, set, oldset)

// ---------------------------------------------------------------------------------------------
// Time zone globals cctz reads; Horizon has no zoneinfo, so these describe UTC.
// ---------------------------------------------------------------------------------------------

#include <cstdio>

extern "C" {
// newlib stores the zone globals under leading-underscore names; cctz reads the glibc spellings.
extern char* _tzname[2];
extern long _timezone;
extern int _daylight;
int isascii(int c);
FILE* fdopen(int fd, const char* mode);
char* strptime(const char* s, const char* format, struct tm* tm);
}

#ifndef tzname
#define tzname _tzname
#endif
#ifndef timezone
#define timezone _timezone
#endif
#ifndef daylight
#define daylight _daylight
#endif
