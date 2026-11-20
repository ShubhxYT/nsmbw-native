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
    WorkerWaitSealed,
    WorkerEncode,
    StagingMap,
    EfbReadbackWait,
    EfbReadbackEncode,
    GxSyncWait,
    PipelineWait,
    GxBeginSample,
    WorkerWaitDone,
    Count,
};

extern "C" void switch_record_frame_phase(SwitchFramePhase phase, uint64_t nanos);
extern "C" void switch_record_dk_gpu_list(uint64_t nanos);
extern "C" void switch_record_dk_fence_wait(uint64_t nanos);
extern "C" uint64_t switch_set_gpu_sample_tag(uint64_t tag);
extern "C" uint64_t switch_gpu_sample_tag();
extern "C" void switch_record_gpu_frame_part(uint64_t tag, uint64_t begin, uint64_t end);

class SwitchGpuFrameSample {
public:
    SwitchGpuFrameSample(uint32_t frame, bool composition, bool native)
        : active_(native && frame % 16 == 0 && switch_frame_profiling_enabled()),
          previous_(active_ ? switch_set_gpu_sample_tag(
                                   ((static_cast<uint64_t>(frame) + 1) << 1) | composition)
                             : 0) {}
    ~SwitchGpuFrameSample() {
        if (active_) switch_set_gpu_sample_tag(previous_);
    }
    SwitchGpuFrameSample(const SwitchGpuFrameSample&) = delete;
    SwitchGpuFrameSample& operator=(const SwitchGpuFrameSample&) = delete;

private:
    bool active_;
    uint64_t previous_;
};

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
