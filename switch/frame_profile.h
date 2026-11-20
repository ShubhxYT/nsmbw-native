#pragma once

#include <cstdint>

extern "C" bool switch_frame_profiling_enabled();
extern "C" void switch_record_gl_replay(uint64_t nanos);
extern "C" void switch_record_gl_finish(uint64_t nanos);
extern "C" void switch_record_efb_encode(uint64_t nanos);
extern "C" void switch_log_render_profile(int frames);
