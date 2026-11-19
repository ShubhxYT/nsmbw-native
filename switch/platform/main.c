// The Switch platform shell: a 1280x720 deko3d swapchain presenting full-screen clear frames, with
// pad input and a log channel. This is the presentation loop the port's platform layer will use; the
// renderer will later draw into it.
#include <switch.h>
#include <deko3d.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define FB_NUM 2
#define FB_WIDTH 1280
#define FB_HEIGHT 720

#define CLEAR_COUNT 8
#define CMDMEMSIZE (16 * 1024)

#ifndef NSMBW_LOG_HOST
#define NSMBW_LOG_HOST "192.168.1.55"
#endif

static int g_log = -1;

static void LogInit(void) {
    g_log = socket(AF_INET, SOCK_STREAM, 0);
    if (g_log < 0) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(28281);
    addr.sin_addr.s_addr = inet_addr(NSMBW_LOG_HOST);
    if (connect(g_log, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(g_log);
        g_log = -1;
    }
}

static void Say(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (g_log >= 0) {
        send(g_log, line, strlen(line), 0);
        send(g_log, "\n", 1, 0);
    }
}

static DkDevice g_device;
static DkMemBlock g_framebufferMemBlock;
static DkImage g_framebuffers[FB_NUM];
static DkSwapchain g_swapchain;
static DkMemBlock g_cmdbufMemBlock;
static DkCmdBuf g_cmdbuf;
static DkCmdList g_cmdsBindFramebuffer[FB_NUM];
static DkCmdList g_cmdsClear[CLEAR_COUNT];
static DkQueue g_renderQueue;

static void graphicsInitialize(void) {
    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    g_device = dkDeviceCreate(&deviceMaker);

    DkImageLayoutMaker imageLayoutMaker;
    dkImageLayoutMakerDefaults(&imageLayoutMaker, g_device);
    imageLayoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    imageLayoutMaker.format = DkImageFormat_RGBA8_Unorm;
    imageLayoutMaker.dimensions[0] = FB_WIDTH;
    imageLayoutMaker.dimensions[1] = FB_HEIGHT;

    DkImageLayout framebufferLayout;
    dkImageLayoutInitialize(&framebufferLayout, &imageLayoutMaker);

    uint32_t framebufferSize = dkImageLayoutGetSize(&framebufferLayout);
    uint32_t framebufferAlign = dkImageLayoutGetAlignment(&framebufferLayout);
    framebufferSize = (framebufferSize + framebufferAlign - 1) & ~(framebufferAlign - 1);

    DkMemBlockMaker memBlockMaker;
    dkMemBlockMakerDefaults(&memBlockMaker, g_device, FB_NUM * framebufferSize);
    memBlockMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_framebufferMemBlock = dkMemBlockCreate(&memBlockMaker);

    DkImage const* swapchainImages[FB_NUM];
    for (unsigned i = 0; i < FB_NUM; ++i) {
        swapchainImages[i] = &g_framebuffers[i];
        dkImageInitialize(&g_framebuffers[i], &framebufferLayout, g_framebufferMemBlock, i * framebufferSize);
    }

    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, g_device, nwindowGetDefault(), swapchainImages, FB_NUM);
    g_swapchain = dkSwapchainCreate(&swapchainMaker);

    dkMemBlockMakerDefaults(&memBlockMaker, g_device, CMDMEMSIZE);
    memBlockMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmdbufMemBlock = dkMemBlockCreate(&memBlockMaker);

    DkCmdBufMaker cmdbufMaker;
    dkCmdBufMakerDefaults(&cmdbufMaker, g_device);
    g_cmdbuf = dkCmdBufCreate(&cmdbufMaker);
    dkCmdBufAddMemory(g_cmdbuf, g_cmdbufMemBlock, 0, CMDMEMSIZE);

    for (unsigned i = 0; i < FB_NUM; ++i) {
        DkImageView imageView;
        dkImageViewDefaults(&imageView, &g_framebuffers[i]);
        dkCmdBufBindRenderTarget(g_cmdbuf, &imageView, NULL);
        g_cmdsBindFramebuffer[i] = dkCmdBufFinishList(g_cmdbuf);
    }

    // Eight clear colours, so the screen visibly changes every frame without needing a shader yet.
    for (unsigned i = 0; i < CLEAR_COUNT; ++i) {
        const float t = (float)i / (float)CLEAR_COUNT;
        dkCmdBufClearColorFloat(g_cmdbuf, 0, DkColorMask_RGBA, 0.10f + 0.5f * t, 0.20f + 0.3f * (1.0f - t),
                                0.45f, 1.0f);
        g_cmdsClear[i] = dkCmdBufFinishList(g_cmdbuf);
    }

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, g_device);
    queueMaker.flags = DkQueueFlags_Graphics;
    g_renderQueue = dkQueueCreate(&queueMaker);
}

int main(int argc, char** argv) {
    socketInitializeDefault();
    LogInit();

    Say("NSMBW switch platform shell");
    Say("swapchain:     %dx%d, %d framebuffers", FB_WIDTH, FB_HEIGHT, FB_NUM);
    Say("operation:     %s",
        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    Say("log channel:   %s", g_log >= 0 ? "connected" : "screen only");

    nwindowSetDimensions(nwindowGetDefault(), FB_WIDTH, FB_HEIGHT);
    graphicsInitialize();
    Say("deko3d:        device, swapchain and colours ready");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    unsigned frame = 0, ticks = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;

        int slot = dkQueueAcquireImage(g_renderQueue, g_swapchain);
        dkQueueSubmitCommands(g_renderQueue, g_cmdsBindFramebuffer[slot]);
        dkQueueSubmitCommands(g_renderQueue, g_cmdsClear[frame % CLEAR_COUNT]);
        dkQueuePresentImage(g_renderQueue, g_swapchain, slot);

        if (++frame % 60 == 0) {
            Say("frame %u", frame);
            ++ticks;
        }
    }

    Say("exiting after %u frames", frame);
    dkQueueWaitIdle(g_renderQueue);
    dkQueueDestroy(g_renderQueue);
    dkCmdBufDestroy(g_cmdbuf);
    dkMemBlockDestroy(g_cmdbufMemBlock);
    dkSwapchainDestroy(g_swapchain);
    dkMemBlockDestroy(g_framebufferMemBlock);
    dkDeviceDestroy(g_device);
    if (g_log >= 0) close(g_log);
    return 0;
}
