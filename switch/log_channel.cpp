// Mirrors the process's stdout/stderr to a TCP socket on the development machine.
//
// hbmenu starts an .nro without a stdio channel, and nxlinkStdio() needs the address hbmenu learned
// from the netloader handshake, which it does not pass on to the app. The port therefore dials the
// host itself: the accompanying nxlink.py listens on this port and prints everything it receives.
//
// The redirection is dup2 onto descriptors 1 and 2, so ordinary printf/fprintf in the runtime and
// the translated game code reach the terminal without any changes on their side.

#include "data_root.h"
#include "frame_log.h"
#include "frame_profile.h"

#include <switch.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include <fcntl.h>

extern "C" int g_switchTlsResult;
extern "C" unsigned long long g_switchTlsDelta;

namespace {

// Where the host listener runs. Override at build time with -DNSMBW_LOG_HOST=\"...\".
#ifndef NSMBW_LOG_HOST
#define NSMBW_LOG_HOST "192.168.1.55"
#endif

#ifndef NSMBW_LOG_PORT
#define NSMBW_LOG_PORT 28281
#endif

int connect_log_channel() {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(NSMBW_LOG_PORT);
    if (inet_pton(AF_INET, NSMBW_LOG_HOST, &address.sin_addr) != 1) {
        return -1;
    }

    // The host listener is started before the push, but the console spends a while loading a large
    // .nro before main() runs; retry for a few seconds instead of failing on the first attempt.
    // Two attempts only: with no listener running, each refused connect costs the host's full
    // timeout, and twenty of them delay startup by ~50 s - long enough to look like a hang.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            break;
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        close(fd);
        svcSleepThread(500000000ll);  // 0.5 s
    }
    return -1;
}

#if defined(NSMBW_USE_DEKO3D)
constexpr const char* kBootLogPath = NSMBW_SWITCH_DATA_ROOT "/boot-deko3d.log";
constexpr const char* kFrameLogPath = NSMBW_SWITCH_DATA_ROOT "/frame-deko3d.log";
#else
constexpr const char* kBootLogPath = NSMBW_SWITCH_DATA_ROOT "/boot.log";
constexpr const char* kFrameLogPath = NSMBW_SWITCH_DATA_ROOT "/frame.log";
#endif

// Keep a log on the SD card when the development machine is not listening.
int open_log_file() {
    ::remove(kBootLogPath);
    return open(kBootLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}

bool write_log_bytes(int fd, const char* data, size_t size) {
    while (size != 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        data += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

class FrameLogWriter {
public:
    void Start() {
        try {
            worker_ = std::jthread([this](std::stop_token stop) { Run(stop); });
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "nsmbw: frame log worker could not start (%s)\n", error.what());
        }
    }

    bool Ready() const { return ready_.load(std::memory_order_acquire); }

    void Append(const char* data, size_t size) {
        std::lock_guard lock(mutex_);
        auto& pending = batches_[writeSlot_];
        if (size > pending.data.size() - pending.size) {
            ++pending.dropped;
            return;
        }
        std::memcpy(pending.data.data() + pending.size, data, size);
        pending.size += size;
    }

private:
    struct Batch {
        std::array<char, 32 * 1024> data;
        size_t size = 0;
        unsigned dropped = 0;
    };

    void Run(std::stop_token stop) {
        const int fd = ::open(kFrameLogPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            std::fprintf(stderr, "nsmbw: could not open %s\n", kFrameLogPath);
            return;
        }
        constexpr char header[] = "nsmbw: background frame log\n";
        if (!write_log_bytes(fd, header, sizeof(header) - 1)) {
            ::close(fd);
            std::fprintf(stderr, "nsmbw: could not initialize %s\n", kFrameLogPath);
            return;
        }
        ready_.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
            svcSleepThread(250'000'000);
            Flush(fd);
        }
        ready_.store(false, std::memory_order_release);
        Flush(fd);
        ::close(fd);
    }

    void Flush(int fd) {
        unsigned slot;
        {
            std::lock_guard lock(mutex_);
            slot = writeSlot_;
            if (batches_[slot].size == 0 && batches_[slot].dropped == 0) {
                return;
            }
            writeSlot_ ^= 1;
        }
        // The frame thread can queue another batch while this thread writes to the SD card.
        auto& batch = batches_[slot];
        if (!write_log_bytes(fd, batch.data.data(), batch.size)) {
            std::fprintf(stderr, "nsmbw: could not write a batch to %s\n", kFrameLogPath);
        } else if (batch.dropped != 0) {
            char message[96];
            const int length = std::snprintf(message, sizeof(message), "nsmbw: dropped %u frame log records\n",
                                             batch.dropped);
            if (length > 0) {
                write_log_bytes(fd, message, static_cast<size_t>(length));
            }
        }
        batch.size = 0;
        batch.dropped = 0;
    }

    std::mutex mutex_;
    std::array<Batch, 2> batches_{};
    unsigned writeSlot_ = 0;
    std::atomic_bool ready_{false};
    std::jthread worker_;
};

FrameLogWriter& frame_log_writer() {
    static FrameLogWriter writer;
    return writer;
}

struct RenderTiming {
    std::atomic<uint64_t> nanos{0};
    std::atomic<uint64_t> calls{0};

    void Add(uint64_t elapsed) {
        nanos.fetch_add(elapsed, std::memory_order_relaxed);
        calls.fetch_add(1, std::memory_order_relaxed);
    }

    double TakeMillisecondsPerFrame(int frames) {
        return nanos.exchange(0, std::memory_order_relaxed) / (1e6 * frames);
    }

    double TakeCallsPerFrame(int frames) {
        return calls.exchange(0, std::memory_order_relaxed) / static_cast<double>(frames);
    }
};

RenderTiming g_glReplay;
RenderTiming g_glFinish;
RenderTiming g_efbEncode;
#if defined(NSMBW_USE_DEKO3D)
std::array<std::atomic<uint64_t>, static_cast<size_t>(SwitchFramePhase::Count)> g_framePhases{};
RenderTiming g_dkFenceWait;
// Sum and count are captured together because the GPU worker and VI report race.
std::mutex g_gpuSamplesMutex;
uint64_t g_gpuSampleNanos = 0;
uint64_t g_gpuSamples = 0;
#endif

} // namespace

namespace SwitchLog {

// Returns true when the terminal was connected, so callers can report the outcome locally.
bool Open() {
    static bool attempted = false;
    if (attempted) {
        return false;
    }
    attempted = true;

    // Write to the SD card first: the file is the reliable channel, and it must exist the moment this
    // function runs. The socket is a bonus on top.
    const int fileFd = open_log_file();
    if (fileFd >= 0) {
        dup2(fileFd, STDOUT_FILENO);
        dup2(fileFd, STDERR_FILENO);
        if (fileFd > STDERR_FILENO) {
            close(fileFd);
        }
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }

    // Then try to mirror to the development machine; failure only means no live stream.
    const int sockFd = NSMBW_LOG_HOST[0] != '\0' && R_SUCCEEDED(socketInitializeDefault())
                           ? connect_log_channel()
                           : -1;
    if (sockFd >= 0) {
        dup2(sockFd, STDOUT_FILENO);
        dup2(sockFd, STDERR_FILENO);
        if (sockFd > STDERR_FILENO) {
            close(sockFd);
        }
    }
    return sockFd >= 0;
}

} // namespace SwitchLog

extern "C" void switch_start_frame_log() {
    frame_log_writer().Start();
}

extern "C" void switch_frame_logf(const char* format, ...) {
    auto& writer = frame_log_writer();
    std::va_list args;
    va_start(args, format);
    if (!writer.Ready()) {
        std::vfprintf(stderr, format, args);
        va_end(args);
        return;
    }
    char line[2048];
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0) {
        const size_t size = std::min(static_cast<size_t>(length), sizeof(line) - 1);
        if (static_cast<size_t>(length) >= sizeof(line)) {
            line[size - 1] = '\n';
        }
        writer.Append(line, size);
    }
}

extern "C" bool switch_frame_profiling_enabled() {
    static const bool enabled = [] {
        const int fd = ::open(NSMBW_SWITCH_DATA_ROOT "/frame-profile.flag", O_RDONLY);
        if (fd < 0) {
            return false;
        }
        close(fd);
        std::fprintf(stderr, "nsmbw: frame profiling enabled\n");
        return true;
    }();
    return enabled;
}

extern "C" void switch_record_gl_replay(uint64_t nanos) { g_glReplay.Add(nanos); }
extern "C" void switch_record_gl_finish(uint64_t nanos) { g_glFinish.Add(nanos); }
extern "C" void switch_record_efb_encode(uint64_t nanos) { g_efbEncode.Add(nanos); }

#if defined(NSMBW_USE_DEKO3D)
extern "C" void switch_record_frame_phase(SwitchFramePhase phase, uint64_t nanos) {
    g_framePhases[static_cast<size_t>(phase)].fetch_add(nanos, std::memory_order_relaxed);
}

extern "C" void switch_record_dk_gpu_list(uint64_t nanos) {
    std::lock_guard lock(g_gpuSamplesMutex);
    g_gpuSampleNanos += nanos;
    ++g_gpuSamples;
}

extern "C" void switch_record_dk_fence_wait(uint64_t nanos) { g_dkFenceWait.Add(nanos); }
#endif

extern "C" void switch_log_render_profile(int frames) {
#if defined(NSMBW_USE_DEKO3D)
    switch_frame_logf("[vi] render profile: deko3d, EFB copy encoding %.2f ms (%.1f copies) per frame\n",
                      g_efbEncode.TakeMillisecondsPerFrame(frames), g_efbEncode.TakeCallsPerFrame(frames));
    const auto takePhase = [frames](SwitchFramePhase phase) {
        return g_framePhases[static_cast<size_t>(phase)].exchange(0, std::memory_order_relaxed) / (1e6 * frames);
    };
    switch_frame_logf("[vi] handheld profile: worker wait %.2f, worker encode %.2f, staging map %.2f, "
                      "EFB readback wait %.2f, EFB RAM encode %.2f ms/frame\n",
                      takePhase(SwitchFramePhase::WorkerWait), takePhase(SwitchFramePhase::WorkerEncode),
                      takePhase(SwitchFramePhase::StagingMap), takePhase(SwitchFramePhase::EfbReadbackWait),
                      takePhase(SwitchFramePhase::EfbReadbackEncode));
    switch_frame_logf("[vi] GX profile: FIFO sync wait %.2f, pipeline wait %.2f ms/frame\n",
                      takePhase(SwitchFramePhase::GxSyncWait), takePhase(SwitchFramePhase::PipelineWait));
    uint64_t sampleNanos;
    uint64_t samples;
    {
        std::lock_guard lock(g_gpuSamplesMutex);
        sampleNanos = std::exchange(g_gpuSampleNanos, 0);
        samples = std::exchange(g_gpuSamples, 0);
    }
    switch_frame_logf("[vi] deko3d profile: fence wait %.2f ms/frame (%.1f waits), "
                      "GPU %.3f ms/sample (%llu completed lists)\n",
                      g_dkFenceWait.TakeMillisecondsPerFrame(frames), g_dkFenceWait.TakeCallsPerFrame(frames),
                      samples ? sampleNanos / (1e6 * samples) : 0.0, static_cast<unsigned long long>(samples));
#else
    switch_frame_logf("[vi] render profile: GL command replay %.2f ms (%.1f batches), glFinish %.2f ms "
                      "(%.1f waits), EFB copy encoding %.2f ms (%.1f copies) per frame\n",
                      g_glReplay.TakeMillisecondsPerFrame(frames), g_glReplay.TakeCallsPerFrame(frames),
                      g_glFinish.TakeMillisecondsPerFrame(frames), g_glFinish.TakeCallsPerFrame(frames),
                      g_efbEncode.TakeMillisecondsPerFrame(frames), g_efbEncode.TakeCallsPerFrame(frames));
#endif
}

// Runs before main(): the runtime writes to stdio during static initialisation in places.
__attribute__((constructor)) static void switch_open_log_channel() { SwitchLog::Open(); }

// Called from main() so a run that gets past every initialiser says so.
extern "C" void switch_trace_mark(const char* tag) {
    if (tag == nullptr) {
        return;
    }
    // Dispatch probes and older objects with the instrumented config getter
    // can generate many marks per frame. Keep only their startup diagnostics.
    if ((tag[0] == 'I' && tag[1] == 'I' && (tag[2] == '>' || tag[2] == '<')) ||
        std::strncmp(tag, "REG: ", 5) == 0 || std::strcmp(tag, "CFG enter") == 0 ||
        std::strcmp(tag, "CFG loaded") == 0) {
        return;
    }
    const int fd = ::open(NSMBW_SWITCH_DATA_ROOT "/init.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    (void)!::write(fd, tag, std::strlen(tag));
    (void)!::write(fd, "\n", 1);
    ::close(fd);
}

// Opens the log channel before any other static initialiser runs. Constructor priority 101 sorts ahead
// of every default-priority (65535) initialiser, and .preinit_array runs before even those, so a crash
// inside a global constructor still leaves evidence of how far the process got.
static void switch_log_earliest() {
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    const bool connected = SwitchLog::Open();
    std::fprintf(stderr, "nsmbw: reached C++ runtime (log %s) tls=%d delta=%llu\n",
                 connected ? "socket" : "file", g_switchTlsResult,
                 static_cast<unsigned long long>(g_switchTlsDelta));
    std::fflush(stderr);
    switch_trace_mark("CRT: logger opened");
}

__attribute__((constructor(101))) static void switch_log_earliest_ctor() { switch_log_earliest(); }

__attribute__((section(".preinit_array"), used))
static void (*g_switch_preinit)(void) = switch_log_earliest;
