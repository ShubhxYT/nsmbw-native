// Probes the C runtime surface the port depends on: printf's %s, the C locale, and std::locale.
// Deliberately avoids <iostream>, whose global initialiser is tested separately (probe.cpp).
#include <switch.h>

#include <fcntl.h>
#include <locale.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <locale>
#include <string>

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

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    mark("A main");

    char buffer[128];

    std::snprintf(buffer, sizeof buffer, "B int=%d", 42);
    mark(buffer);

    std::snprintf(buffer, sizeof buffer, "C str=[%s]", "abc");
    mark(buffer);

    std::snprintf(buffer, sizeof buffer, "D char=%c", 'z');
    mark(buffer);

    const char* setResult = setlocale(LC_ALL, "C");
    mark(setResult != nullptr ? "E setlocale ok" : "E setlocale null");

    const locale_t loc = newlocale(LC_ALL_MASK, "C", nullptr);
    mark(loc != nullptr ? "F newlocale ok" : "F newlocale null");
    if (loc != nullptr) {
        freelocale(loc);
    }

    mark("G before std::locale::classic");
    const std::locale& classic = std::locale::classic();
    mark("H after std::locale::classic");

    std::string text = "I std::string";
    mark(text.c_str());

    mark("J done");
    svcSleepThread(1000000000ll);
    return 0;
}
