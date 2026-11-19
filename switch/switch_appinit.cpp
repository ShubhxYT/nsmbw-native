// Replaces libnx's default startup for the Switch port.
//
// libnx's own __appInit aborts the process when any default service fails to initialise. On a title
// installed from an NSP that happens for services the NPDM does not grant (hid, most notably), and
// the abort is fatal before main() ever runs. __appInit is weak, so this definition takes over and
// brings up exactly what the port needs, treating each optional service as optional.
#include <switch.h>

#include <cstdio>

extern "C" void __appInit(void) {
    // Base IPC. Without this nothing else can be reached.
    smInitialize();

    // Filesystem: the game data and logs live on the SD card.
    if (R_SUCCEEDED(fsInitialize())) {
        fsdevMountSdmc();
    }

    // Applet state, so the system knows the process is a running application (and so nwindow works).
    appletInitialize();

    // Input. Denied by some NPDMs; the game still renders without a controller.
    const Result hid = hidInitialize();
    if (R_FAILED(hid)) {
        std::fprintf(stderr, "[switch] hidInitialize failed: 0x%x (no controller input)\n", hid);
        std::fflush(stderr);
    }

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
