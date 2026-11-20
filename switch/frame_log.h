#pragma once

extern "C" void switch_start_frame_log();
extern "C" void switch_frame_logf(const char* format, ...) __attribute__((format(printf, 1, 2)));

#define RT_FRAME_LOGF(tag, ...) switch_frame_logf("[" tag "] " __VA_ARGS__)
