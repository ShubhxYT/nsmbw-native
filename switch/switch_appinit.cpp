// Replaces libnx's default startup for the Switch port.
//
// libnx's own __appInit aborts the process when any default service fails to initialise. On a title
// installed from an NSP that happens for services the NPDM does not grant (hid, most notably), and
// the abort is fatal before main() ever runs. __appInit is weak, so this definition takes over and
// brings up exactly what the port needs, treating each optional service as optional.
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include "switch_tls.h"

// Definitions for the bootstrap's reporting state (declared in switch_tls.h).
extern "C" int g_switchTlsResult = -1;
extern "C" uint64_t g_switchTlsDelta = 0;
extern "C" thread_local uint64_t g_switchTlsAnchor = kSwitchTlsSentinel;

extern "C" void __appInit(void) {
    // First: C++ thread_local is unusable until tpidr_el0 points at the TLS block. Nothing in libnx
    // sets it (libnx reads tpidrro_el0, the IPC buffer), so any thread_local - toml11's parser spec,
    // libstdc++ internals - dereferences null and dies as "Fault Address: 0".
    SwitchTlsBootstrap();

    // Base IPC. Without this nothing else can be reached.
    smInitialize();

    // Filesystem: the game data and logs live on the SD card.
    if (R_SUCCEEDED(fsInitialize())) {
        fsdevMountSdmc();
    }

    // Applet state, so the system knows the process is a running application (and so nwindow works).
    appletInitialize();

    // Input. Denied by some NPDMs; the game still renders without a controller.
    // No stdio here: this runs inside crt0, before libc's streams exist. Touching stderr aborts.
    hidInitialize();

    // Clock, used for timing and for the crash reporter's timestamps.
    timeInitialize();

    // Diagnostic: .bss must be zero here, before any C++ static initialiser runs. A non-zero word
    // means function-local statics never get constructed, because their guard variables read as
    // "already initialised". Written with raw write(2): stdio is not up yet.
    {
        extern char __bss_start__[] __attribute__((weak));
        if (__bss_start__ != nullptr) {
            volatile unsigned* words = reinterpret_cast<volatile unsigned*>(__bss_start__);
            char line[128];
            int n = std::snprintf(line, sizeof line, "BSS@appInit: %08x %08x %08x %08x",
                                  words[0], words[16], words[48], words[256]);
            if (n > 0) {
                int fd = open("sdmc:/switch/nsmbw/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd >= 0) {
                    (void)!write(fd, line, static_cast<size_t>(n));
                    (void)!write(fd, "\n", 1);
                    close(fd);
                }
            }
        }
    }
}

extern "C" void __appExit(void) {
    timeExit();
    hidExit();
    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
