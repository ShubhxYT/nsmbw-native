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
#include <sys/stat.h>
#include <unistd.h>

#include "data_root.h"
#include "switch_tls.h"

// Definitions for the bootstrap's reporting state (declared in switch_tls.h).
extern "C" int g_switchTlsResult = -1;
extern "C" uint64_t g_switchTlsDelta = 0;
extern "C" thread_local uint64_t g_switchTlsAnchor = kSwitchTlsSentinel;

namespace {

ViDisplay mainDisplay{};
ViLayer mainLayer{};
NWindow mainWindow{};
bool viReady = false;
bool displayOpen = false;
bool layerOpen = false;

void logWindowInit(const char* step, Result result) {
    char line[96];
    const int size = std::snprintf(line, sizeof(line), "Window init: %s result=0x%08x\n", step,
                                   static_cast<unsigned>(result));
    if (size <= 0 || static_cast<size_t>(size) >= sizeof(line)) {
        return;
    }
    const int fd = open(NSMBW_SWITCH_DATA_ROOT "/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!write(fd, line, static_cast<size_t>(size));
        close(fd);
    }
}

void closeMainWindow() {
    nwindowClose(&mainWindow);
    if (layerOpen) {
        viCloseLayer(&mainLayer);
        layerOpen = false;
    }
    if (displayOpen) {
        viCloseDisplay(&mainDisplay);
        displayOpen = false;
    }
    if (viReady) {
        viExit();
        viReady = false;
    }
}

void openMainWindow() {
    // The __appInit override replaces libnx's default window setup.
    Result result = viInitialize(ViServiceType_Application);
    logWindowInit("viInitialize", result);
    if (R_FAILED(result)) {
        return;
    }
    viReady = true;

    result = viOpenDefaultDisplay(&mainDisplay);
    logWindowInit("viOpenDefaultDisplay", result);
    if (R_FAILED(result)) {
        closeMainWindow();
        return;
    }
    displayOpen = true;

    result = viCreateLayer(&mainDisplay, &mainLayer);
    logWindowInit("viCreateLayer", result);
    if (R_FAILED(result)) {
        closeMainWindow();
        return;
    }
    layerOpen = true;

    result = nwindowCreateFromLayer(&mainWindow, &mainLayer);
    logWindowInit("nwindowCreateFromLayer", result);
    if (R_FAILED(result)) {
        closeMainWindow();
    }
}

} // namespace

extern "C" NWindow* switchGetMainWindow() {
    if (nwindowIsValid(&mainWindow)) {
        return &mainWindow;
    }
    NWindow* fallback = nwindowGetDefault();
    return nwindowIsValid(fallback) ? fallback : nullptr;
}

extern "C" void __appInit(void) {
    // First: C++ thread_local is unusable until tpidr_el0 points at the TLS block. Nothing in libnx
    // sets it (libnx reads tpidrro_el0, the IPC buffer), so any thread_local - toml11's parser spec,
    // libstdc++ internals - dereferences null and dies as "Fault Address: 0".
    SwitchTlsBootstrap();

    // Base IPC. Without this nothing else can be reached.
    smInitialize();

    // Create writable storage before the startup loggers and config load run.
    if (R_SUCCEEDED(fsInitialize()) && R_SUCCEEDED(fsdevMountSdmc())) {
        (void)::mkdir("sdmc:/switch", 0777);
        (void)::mkdir(NSMBW_SWITCH_DATA_ROOT, 0777);
    }

    // Applet state, so the system knows the process is a running application (and so nwindow works).
    logWindowInit("appletInitialize", appletInitialize());
    openMainWindow();

    // Input. Denied by some NPDMs; the game still renders without a controller.
    // No stdio here: this runs inside crt0, before libc's streams exist. Touching stderr aborts.
    hidInitialize();

    // Frame timing uses the hardware counter. Leave time-service clock objects available to
    // processes such as Ultrahand that need a wall clock while the game is running.

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
                int fd = open(NSMBW_SWITCH_DATA_ROOT "/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
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
    hidExit();
    closeMainWindow();
    appletExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}


// RomFS holds the read-only package data. Writable files stay on the SD card.
extern "C" bool switch_mount_shader_seed() {
    static const bool mounted = R_SUCCEEDED(romfsInit());
    return mounted;
}

extern "C" bool switch_has_bundled_game() {
    static const bool bundled = [] {
        struct stat info{};
        return switch_mount_shader_seed() &&
               ::stat(NSMBW_SWITCH_BUNDLED_GAME_ROOT "/sys/boot.bin", &info) == 0 && S_ISREG(info.st_mode);
    }();
    return bundled;
}
