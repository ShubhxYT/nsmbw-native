// Minimal NSP payload. Proves packaging, the NPDM and the loader path independently of the game.
//
// __appInit is a weak symbol in libnx: this definition replaces the default startup, which aborts
// when a service the NPDM does not grant is initialised (hid, in our case). Only sm and fs are
// needed to write a file, so only those are initialised.
#include <switch.h>

#include <cstdio>

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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    FILE* out = std::fopen("sdmc:/switch/nsmbw/hello.txt", "w");
    if (out != nullptr) {
        std::fprintf(out, "hello from nsp\n");
        std::fclose(out);
    }
    svcSleepThread(2000000000ll);
    return 0;
}
