#pragma once

#include <cstdint>

extern "C" bool switch_frame_profiling_enabled();
extern "C" void switch_record_gl_replay(uint64_t nanos);
extern "C" void switch_record_gl_finish(uint64_t nanos);
extern "C" void switch_record_efb_encode(uint64_t nanos);
extern "C" void switch_log_render_profile(int frames);

#if defined(NSMBW_USE_DEKO3D)
#include <chrono>

enum class SwitchFramePhase {
    WorkerWait,
    WorkerEncode,
    StagingMap,
    EfbReadbackWait,
    EfbReadbackEncode,
    GxSyncWait,
    PipelineWait,
    Count,
};

extern "C" void switch_record_frame_phase(SwitchFramePhase phase, uint64_t nanos);
extern "C" void switch_record_dk_gpu_list(uint64_t nanos);
extern "C" void switch_record_dk_fence_wait(uint64_t nanos);

class SwitchFrameTimer {
public:
    explicit SwitchFrameTimer(SwitchFramePhase phase, bool measure = true)
        : phase_(phase),
          start_(measure && switch_frame_profiling_enabled() ? Clock::now() : Clock::time_point{}) {}

    ~SwitchFrameTimer() {
        if (start_ != Clock::time_point{}) {
            switch_record_frame_phase(
                phase_, std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count());
        }
    }

    SwitchFrameTimer(const SwitchFrameTimer&) = delete;
    SwitchFrameTimer& operator=(const SwitchFrameTimer&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    SwitchFramePhase phase_;
    Clock::time_point start_;
};
#endif
