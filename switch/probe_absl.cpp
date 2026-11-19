// Reproduces Abseil's flag registration, which is where the game's static initialisation now stops.
// A single ABSL_FLAG definition registers itself before main, exactly like absl/flags/flag.cc does.
#include <switch.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "absl/flags/flag.h"

#include "switch_tls.h"

extern "C" thread_local uint64_t g_switchTlsAnchor = kSwitchTlsSentinel;
extern "C" int g_switchTlsResult = -1;
extern "C" uint64_t g_switchTlsDelta = 0;

ABSL_FLAG(std::string, probe_flag, "", "probe flag");

extern "C" void __appInit(void) {
    SwitchTlsBootstrap();
    smInitialize();
    fsInitialize();
    fsdevMountSdmc();
}

extern "C" void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}

namespace {
void mark(const char* tag) {
    const int fd = open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    (void)!write(fd, tag, strlen(tag));
    (void)!write(fd, "\n", 1);
    close(fd);
}
} // namespace

extern "C" void switch_trace_mark(const char* tag) { mark(tag); }

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    mark("A main");
    mark(g_switchTlsResult == 1 ? "B tls ok" : "B tls failed");
    mark(absl::GetFlag(FLAGS_probe_flag).empty() ? "C flag read ok" : "C flag read odd");
    mark("D done");
    svcSleepThread(1000000000ll);
    return 0;
}
