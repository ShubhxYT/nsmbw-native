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

// libnx applies the module's dynamic relocations itself (__nx_dynamic -> _dynProcessRela/Relr). That
// walker faults on our image, dying before main(). The system loader has already relocated an
// installed title, so make the library's pass a no-op and let execution continue.
// __nx_dynamic is NOT overridden any more: libnx must apply the module's relocations. The reason it
// used to fault is that --spare-dynamic-tags=0 leaves no room for the DT_NULL terminator, so the tag
// loop ran past the end of .dynamic. The link now passes -z spare-dynamic-tags=1.
