// Phase 0 for the Switch port: prove the toolchain, the console and the screen before any of the
// game exists. Prints what the applet reports about itself and waits for + to exit.
#include <switch.h>
#include <stdio.h>

int main(int argc, char **argv) {
    consoleInit(NULL);

    printf("\x1b[2J\x1b[H");
    printf("NSMBW native - Switch toolchain check\n\n");

    const u32 version = hosversionGet();
    printf("firmware:      %u.%u.%u\n", (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF);
    printf("operation:     %s\n",
           appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    printf("applet type:   %s\n",
           appletGetAppletType() == AppletType_Application ? "application" : "applet (hbmenu)");

    // The Switch Lite panel is 1280x720, so a 720p render is native resolution there.
    printf("target:        1280x720 (Switch Lite handheld)\n");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    printf("pad:           read ok\n");

    printf("\nthis build proves: devkitA64 + libnx link, .nro runs, framebuffer works.\n");
    printf("next: deko3d triangle, then the game.\n\n");
    printf("press + to exit\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
    return 0;
}
