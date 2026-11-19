// The runtime leans on two things Horizon does not obviously have: C++ threads (36 sites) and
// std::filesystem (47 files). This probe runs both for real and reports what worked.
#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

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
    printf("%s\n", line);
    if (g_log >= 0) {
        send(g_log, line, strlen(line), 0);
        send(g_log, "\n", 1, 0);
    }
}

int main(int argc, char** argv) {
    socketInitializeDefault();
    LogInit();
    consoleInit(NULL);
    printf("\x1b[2J\x1b[H");

    Say("NSMBW runtime prerequisites probe");

    // --- threads -----------------------------------------------------------------
    std::atomic<int> counter{0};
    const auto threadStart = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> pool;
        for (int i = 0; i < 4; ++i) {
            pool.emplace_back([&counter] {
                for (int n = 0; n < 100000; ++n) counter.fetch_add(1, std::memory_order_relaxed);
            });
        }
        for (auto& t : pool) t.join();
    }
    const auto threadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - threadStart).count();
    Say("threads:       4 x 100000 increments -> %d (%lld ms)", counter.load(), (long long)threadMs);

    // --- chrono ------------------------------------------------------------------
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto slept = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
    Say("chrono sleep:  50 ms requested, %lld ms measured", (long long)slept);

    // --- filesystem --------------------------------------------------------------
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = "sdmc:/switch/nsmbw-probe";
    fs::create_directories(dir, ec);
    Say("fs mkdir:      %s", ec ? ("failed: " + ec.message()).c_str() : "ok");

    const fs::path file = dir / "hello.txt";
    {
        std::FILE* f = std::fopen(file.string().c_str(), "wb");
        if (f) {
            std::fputs("written by the Switch probe\n", f);
            std::fclose(f);
        }
    }
    Say("fs write:      %s", fs::exists(file, ec) ? "ok" : "failed");
    Say("fs size:       %lld bytes", (long long)fs::file_size(file, ec));

    // --- directory iteration (what the runtime does a lot of) --------------------
    int entries = 0;
    for (const auto& entry : fs::directory_iterator("sdmc:/switch", ec)) {
        (void)entry;
        ++entries;
    }
    Say("fs iterate:    %d entries in sdmc:/switch", entries);

    Say("");
    Say("press + to exit");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    unsigned frames = 0, ticks = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (++frames >= 60) { frames = 0; Say("alive x%u", ++ticks); }
        consoleUpdate(NULL);
    }
    if (g_log >= 0) close(g_log);
    consoleExit(NULL);
    return 0;
}
