// Mirrors the process's stdout/stderr to a TCP socket on the development machine.
//
// hbmenu starts an .nro without a stdio channel, and nxlinkStdio() needs the address hbmenu learned
// from the netloader handshake, which it does not pass on to the app. The port therefore dials the
// host itself: the accompanying nxlink.py listens on this port and prints everything it receives.
//
// The redirection is dup2 onto descriptors 1 and 2, so ordinary printf/fprintf in the runtime and
// the translated game code reach the terminal without any changes on their side.

#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>

extern "C" int g_switchTlsResult;
extern "C" unsigned long long g_switchTlsDelta;

// Linker-provided .bss bounds (switch.ld). Must be declared at file scope with C linkage: a
// declaration inside a function or an anonymous namespace gets internal linkage and never resolves.
extern "C" char __bss_start__[] __attribute__((weak));
extern "C" char __bss_end__[] __attribute__((weak));

namespace {

// Where the host listener runs. Override at build time with -DNSMBW_LOG_HOST=\"...\".
#ifndef NSMBW_LOG_HOST
#define NSMBW_LOG_HOST "192.168.1.55"
#endif

#ifndef NSMBW_LOG_PORT
#define NSMBW_LOG_PORT 28281
#endif

int connect_log_channel() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(NSMBW_LOG_PORT);
    if (inet_pton(AF_INET, NSMBW_LOG_HOST, &address.sin_addr) != 1) {
        return -1;
    }

    // The host listener is started before the push, but the console spends a while loading a large
    // .nro before main() runs; retry for a few seconds instead of failing on the first attempt.
    for (int attempt = 0; attempt < 20; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            break;
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        svcSleepThread(500000000ll);  // 0.5 s
    }
    return -1;
}

// Fallback when the development machine is not listening: keep the log on the SD card so a run can
// be inspected even without the network.
int open_log_file() {
    const char* path = "sdmc:/switch/nsmbw/boot.log";
    ::remove(path);
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

} // namespace

namespace SwitchLog {

// Returns true when the terminal was connected, so callers can report the outcome locally.
bool Open() {
    static bool attempted = false;
    if (attempted) {
        return false;
    }
    attempted = true;

    // BSD sockets need the service initialised before the first socket() call.
    socketInitializeDefault();

    // Write to the SD card first: the file is the reliable channel, and it must exist the moment this
    // function runs. The socket is a bonus on top.
    const int fileFd = open_log_file();
    if (fileFd >= 0) {
        dup2(fileFd, STDOUT_FILENO);
        dup2(fileFd, STDERR_FILENO);
        if (fileFd > STDERR_FILENO) {
            close(fileFd);
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }

    // Then try to mirror to the development machine; failure only means no live stream.
    const int sockFd = connect_log_channel();
    if (sockFd >= 0) {
        dup2(sockFd, STDOUT_FILENO);
        dup2(sockFd, STDERR_FILENO);
        if (sockFd > STDERR_FILENO) {
            close(sockFd);
        }
    }
    return true;
}

} // namespace SwitchLog

// Runs before main(): the runtime writes to stdio during static initialisation in places.
__attribute__((constructor)) static void switch_open_log_channel() { SwitchLog::Open(); }

// ---------------------------------------------------------------------------------------------
// TEMPORARY: per-constructor progress trace.
//
// The port stops after the priority-101 logger with no further output, and nothing in the image
// reports which static initialiser it died in. .preinit_array runs before .init_array, so the hook
// below can replace every .init_array slot with a trampoline that records the slot index and then
// tail-calls the original initialiser. The last index written to init.log is the initialiser that
// never returned. Written with raw write(2) so no stdio state is involved.
// ---------------------------------------------------------------------------------------------
extern "C" char __init_array_start[] __attribute__((weak));
extern "C" char __init_array_end[] __attribute__((weak));

namespace {

constexpr int kMaxInitEntries = 512;
void (*g_initTarget[kMaxInitEntries])(void);
int g_initCount = 0;
int g_initNext = 0;
int g_initTraceFd = -1;

void init_trace_index(int value) {
    char digits[16];
    int count = 0;
    if (value == 0) {
        digits[count++] = '0';
    }
    while (value > 0 && count < 16) {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    char buffer[24];
    int out = 0;
    for (int i = count - 1; i >= 0; --i) {
        buffer[out++] = digits[i];
    }
    buffer[out++] = '\n';
    // Open and close per line: the emulator's filesystem caches open handles, and a write that is
    // still buffered when the guest dies would hide the very initialiser being hunted.
    const int fd = ::open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!::write(fd, buffer, static_cast<size_t>(out));
        ::close(fd);
    }
}

// Writes "<index>=0x<value>\n" with write(2) only: this runs between constructors, where stdio
// state is not guaranteed.
void init_trace_pair(int index, unsigned value) {
    char buffer[48];
    int out = 0;
    if (index == 0) {
        buffer[out++] = '0';
    } else {
        char digits[12];
        int count = 0;
        int remaining = index < 0 ? -index : index;
        while (remaining > 0 && count < 12) {
            digits[count++] = static_cast<char>('0' + remaining % 10);
            remaining /= 10;
        }
        if (index < 0) {
            digits[count++] = '-';
        }
        while (count > 0) {
            buffer[out++] = digits[--count];
        }
    }
    buffer[out++] = '=';
    buffer[out++] = '0';
    buffer[out++] = 'x';
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const int digit = static_cast<int>((value >> shift) & 0xF);
        if (digit != 0 || started || shift == 0) {
            started = true;
            buffer[out++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
        }
    }
    buffer[out++] = '\n';
    const int fd = ::open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!::write(fd, buffer, static_cast<size_t>(out));
        ::close(fd);
    }
}

// The word at __bss_start__+0xC0 is TranslatedFunctionRegistry's vector storage. .bss is provably
// zero at __appInit, so whichever constructor changes this word is the one writing 1 into it.
unsigned g_lastBssWord = 0;

unsigned observe_bss_word() {
    if (__bss_start__ == nullptr) {
        return 0;
    }
    const volatile unsigned* word =
        reinterpret_cast<const volatile unsigned*>(__bss_start__ + 0xC0);
    return *word;
}

void init_trampoline() {
    const int index = g_initNext++;
    init_trace_index(index);
    if (index >= 0 && index < g_initCount) {
        g_initTarget[index]();
    }
    const unsigned value = observe_bss_word();
    if (value != g_lastBssWord) {
        init_trace_pair(index, value);
        g_lastBssWord = value;
    }
}

void install_init_trace() {
    if (__init_array_start == nullptr || __init_array_end == nullptr) {
        return;
    }
    g_initTraceFd = ::open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (g_initTraceFd < 0) {
        return;
    }
    auto** entries = reinterpret_cast<void (**)(void)>(__init_array_start);
    const auto total =
        (__init_array_end - __init_array_start) / static_cast<long>(sizeof(void*));
    g_initCount = total < kMaxInitEntries ? static_cast<int>(total) : kMaxInitEntries;
    for (int i = 0; i < g_initCount; ++i) {
        g_initTarget[i] = entries[i];
        entries[i] = &init_trampoline;
    }
}

} // namespace

// Called from main() so a run that gets past every initialiser says so.
extern "C" void switch_trace_mark(const char* tag) {
    if (tag == nullptr) {
        return;
    }
    const int fd = ::open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    (void)!::write(fd, tag, std::strlen(tag));
    (void)!::write(fd, "\n", 1);
    ::close(fd);
}

// Opens the log channel before any other static initialiser runs. Constructor priority 101 sorts ahead
// of every default-priority (65535) initialiser, and .preinit_array runs before even those, so a crash
// inside a global constructor still leaves evidence of how far the process got.
static void switch_log_earliest() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const bool connected = SwitchLog::Open();
    std::fprintf(stderr, "nsmbw: reached C++ runtime (log %s) tls=%d delta=%llu\n",
                 connected ? "socket" : "file", g_switchTlsResult,
                 static_cast<unsigned long long>(g_switchTlsDelta));
    std::fflush(stderr);
    install_init_trace();
}

__attribute__((constructor(101))) static void switch_log_earliest_ctor() { switch_log_earliest(); }

__attribute__((section(".preinit_array"), used))
static void (*g_switch_preinit)(void) = switch_log_earliest;
