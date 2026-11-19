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
    // Two attempts only: with no listener running, each refused connect costs the host's full
    // timeout, and twenty of them delay startup by ~50 s - long enough to look like a hang.
    for (int attempt = 0; attempt < 2; ++attempt) {
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
    const int sockFd = NSMBW_LOG_HOST[0] != '\0' && R_SUCCEEDED(socketInitializeDefault())
                           ? connect_log_channel()
                           : -1;
    if (sockFd >= 0) {
        dup2(sockFd, STDOUT_FILENO);
        dup2(sockFd, STDERR_FILENO);
        if (sockFd > STDERR_FILENO) {
            close(sockFd);
        }
    }
    return sockFd >= 0;
}

} // namespace SwitchLog

// Runs before main(): the runtime writes to stdio during static initialisation in places.
__attribute__((constructor)) static void switch_open_log_channel() { SwitchLog::Open(); }

// Called from main() so a run that gets past every initialiser says so.
extern "C" void switch_trace_mark(const char* tag) {
    if (tag == nullptr) {
        return;
    }
    // Dispatch probes and older objects with the instrumented config getter
    // can generate many marks per frame. Keep only their startup diagnostics.
    if ((tag[0] == 'I' && tag[1] == 'I' && (tag[2] == '>' || tag[2] == '<')) ||
        std::strncmp(tag, "REG: ", 5) == 0 || std::strcmp(tag, "CFG enter") == 0 ||
        std::strcmp(tag, "CFG loaded") == 0) {
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
    switch_trace_mark("CRT: logger opened");
}

__attribute__((constructor(101))) static void switch_log_earliest_ctor() { switch_log_earliest(); }

__attribute__((section(".preinit_array"), used))
static void (*g_switch_preinit)(void) = switch_log_earliest;
