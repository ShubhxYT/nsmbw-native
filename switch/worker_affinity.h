#pragma once

#include <switch.h>

// Keep the game on its default application core and give the two graphics workers separate cores.
inline bool SwitchPinGraphicsWorker(unsigned core) {
    u64 allowed = 0;
    constexpr u64 kApplicationCores = 0b111;
    if (R_FAILED(svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
        (allowed & kApplicationCores) != kApplicationCores) {
        return false;
    }
    return R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core));
}
