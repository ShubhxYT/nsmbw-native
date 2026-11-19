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

    int fd = connect_log_channel();
    if (fd < 0) {
        fd = open_log_file();
        if (fd < 0) {
            return false;
        }
    }

    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) {
        close(fd);
    }
    // Unbuffered: a crash must not swallow the last lines.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

} // namespace SwitchLog

// Runs before main(): the runtime writes to stdio during static initialisation in places.
__attribute__((constructor)) static void switch_open_log_channel() { SwitchLog::Open(); }
