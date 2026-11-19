// Horizon-specific definitions the ported libraries expect but do not provide.
//
// Aurora declares the GameCube cache helpers in its OS headers; the implementations normally come
// from the runtime's guest OS layer, which only defines the versioned (translated) entry points.
// On Switch the guest heap is ordinary AArch64 memory, so a clean-by-VA plus a barrier is the
// correct equivalent of the PowerPC dcbf/dcbst instructions.
#include <cstdint>

extern "C" {

void DCFlushRange(void* addr, uint32_t nBytes) {
    auto* p = static_cast<char*>(addr);
    for (uint32_t i = 0; i < nBytes; i += 64) {
        asm volatile("dc cvau, %0" ::"r"(p + i) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

void DCStoreRange(void* addr, uint32_t nBytes) {
    auto* p = static_cast<char*>(addr);
    for (uint32_t i = 0; i < nBytes; i += 64) {
        asm volatile("dc civac, %0" ::"r"(p + i) : "memory");
    }
    asm volatile("dsb ish" ::: "memory");
}

void DCInvalidateRange(void* addr, uint32_t nBytes) { DCFlushRange(addr, nBytes); }

void DCFlushRangeNoSync(void* addr, uint32_t nBytes) { DCFlushRange(addr, nBytes); }

void DCStoreRangeNoSync(void* addr, uint32_t nBytes) { DCStoreRange(addr, nBytes); }

void ICInvalidateRange(void* addr, uint32_t nBytes) {
    auto* p = static_cast<char*>(addr);
    for (uint32_t i = 0; i < nBytes; i += 64) {
        asm volatile("ic ivau, %0" ::"r"(p + i) : "memory");
    }
    asm volatile("dsb ish\nisb" ::: "memory");
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// POSIX surface for the vendored libraries (see switch/include/switch_posix.h)
// ---------------------------------------------------------------------------------------------

#include <cstdio>
#include <cstring>

#include <switch.h>
#include <map>
#include <mutex>
#include <string>

namespace {
std::mutex g_envMutex;
std::map<std::string, std::string> g_environment;

// A sigset_t on Horizon is just a bitfield that nothing reads.
struct SigSet {
    unsigned long bits[16];
};
} // namespace

extern "C" {

int setenv(const char* name, const char* value, int overwrite) {
    if (name == nullptr || value == nullptr || name[0] == '\0' || strchr(name, '=') != nullptr) {
        return -1;
    }
    std::lock_guard lock(g_envMutex);
    const auto it = g_environment.find(name);
    if (it != g_environment.end() && overwrite == 0) {
        return 0;
    }
    g_environment[name] = value;
    return 0;
}

int unsetenv(const char* name) {
    if (name == nullptr) {
        return -1;
    }
    std::lock_guard lock(g_envMutex);
    g_environment.erase(name);
    return 0;
}

int putenv(char* string) {
    if (string == nullptr) {
        return -1;
    }
    char* equals = strchr(string, '=');
    if (equals == nullptr) {
        return unsetenv(string);
    }
    const std::string name(string, static_cast<size_t>(equals - string));
    return setenv(name.c_str(), equals + 1, 1);
}

int switch_sigfillset(void* set) {
    if (set != nullptr) {
        auto* bits = static_cast<SigSet*>(set);
        for (auto& word : bits->bits) {
            word = ~0ul;
        }
    }
    return 0;
}

int switch_sigemptyset(void* set) {
    if (set != nullptr) {
        auto* bits = static_cast<SigSet*>(set);
        for (auto& word : bits->bits) {
            word = 0;
        }
    }
    return 0;
}

int switch_sigaddset(void* set, int signum) {
    if (set != nullptr && signum > 0) {
        auto* bits = static_cast<SigSet*>(set);
        bits->bits[(signum - 1) / 64] |= 1ul << ((signum - 1) % 64);
    }
    return 0;
}

int switch_pthread_sigmask(int how, const void* set, void* oldset) {
    (void)how;
    (void)set;
    if (oldset != nullptr) {
        switch_sigemptyset(oldset);
    }
    return 0;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// More POSIX gaps used by Dawn's vendored Abseil
// ---------------------------------------------------------------------------------------------

extern "C" {

int strerror_r(int errnum, char* buf, size_t buflen) {
    if (buf == nullptr || buflen == 0) {
        return -1;
    }
    const char* message = std::strerror(errnum);
    if (message == nullptr) {
        message = "Unknown error";
    }
    std::snprintf(buf, buflen, "%s", message);
    return 0;
}

int getpid(void) { return 1; }

int usleep(useconds_t microseconds) {
    svcSleepThread(static_cast<int64_t>(microseconds) * 1000ll);
    return 0;
}

uid_t geteuid(void) { return 0; }

gid_t getegid(void) { return 0; }

uid_t getuid(void) { return 0; }



} // extern "C"

// ---------------------------------------------------------------------------------------------
// UTC-only time zone globals (see switch/include/switch_posix.h)
// ---------------------------------------------------------------------------------------------

extern "C" {


int isascii(int c) {
    const unsigned int value = static_cast<unsigned int>(c);
    return value < 128u ? 1 : 0;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Horizon has no pipe(2); Dawn's event pipe degrades to polling (see SystemEvent.cpp).
// ---------------------------------------------------------------------------------------------

extern "C" int pipe(int fds[2]) {
    if (fds != nullptr) {
        fds[0] = -1;
        fds[1] = -1;
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------
// File ownership has no meaning on Horizon's FAT filesystem.
// ---------------------------------------------------------------------------------------------

extern "C" int fchown(int fd, uid_t owner, gid_t group) {
    (void)fd;
    (void)owner;
    (void)group;
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Horizon has no child processes.
// ---------------------------------------------------------------------------------------------

extern "C" int waitpid(int pid, int* status, int options) {
    (void)pid;
    (void)options;
    if (status != nullptr) {
        *status = 0;
    }
    return -1;
}

// ---------------------------------------------------------------------------------------------
// Mbed TLS platform hooks (built with MBEDTLS_PLATFORM_MS_TIME_ALT).
// ---------------------------------------------------------------------------------------------

extern "C" long long mbedtls_ms_time(void) {
    return static_cast<long long>(armGetSystemTick() / 12500ull);
}
