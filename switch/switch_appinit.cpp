// Replaces libnx's default startup for the Switch port.
//
// libnx's own __appInit aborts the process when any default service fails to initialise. On a title
// installed from an NSP that happens for services the NPDM does not grant (hid, most notably), and
// the abort is fatal before main() ever runs. __appInit is weak, so this definition takes over and
// brings up exactly what the port needs, treating each optional service as optional.
#include <switch.h>

#include <cstdio>

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
}

extern "C" void __appExit(void) {
    timeExit();
    hidExit();
    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}
