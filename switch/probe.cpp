// Minimal probe: does the C++ runtime start, and where does it stop?
// Two variants are built from this file: NOMARK_IOSTREAM off/on, so a hang can be attributed to
// libstdc++'s iostream global initialiser rather than to anything the game does.
#include <switch.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#ifdef PROBE_IOSTREAM
#include <iostream>
#endif

extern "C" void __appInit(void) {
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
int g_fd = -1;

void mark(const char* text) {
    if (g_fd < 0) {
        g_fd = open("sdmc:/switch/nsmbw/probe.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    if (g_fd >= 0) {
        (void)!write(g_fd, text, strlen(text));
        (void)!write(g_fd, "\n", 1);
    }
}
} // namespace

// Runs before .init_array, so it proves the preinit hook is reached.
__attribute__((section(".preinit_array"), used))
static void (*g_probe_preinit)(void) = [] {
    mark("0 preinit");
};

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    mark("1 main");

    // %s through stdio: the game's logger prints a blank here, so check whether it is the runtime.
    std::fprintf(stderr, "probe: %s\n", "literal");
    std::fflush(stderr);

    const char* which = "literal";
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "2 snprintf %s", which);
    mark(buffer);

#ifdef PROBE_IOSTREAM
    std::cout << "3 iostream" << std::endl;
    mark("4 after iostream");
#endif

    mark("5 done");
    svcSleepThread(1000000000ll);
    return 0;
}
