#pragma once

#include "memory.h"
#include "netplay/game_menus.h"

inline bool SwitchUsesMenuButtons() {
    constexpr uint16_t kStage = 0x005;
    constexpr uint32_t kStageGameMode = 0x8042A204u;
    constexpr uint32_t kTitle = 2, kTitleReplay = 3;
    if (GameMenus::CurrentScene() != kStage) {
        return true;
    }
    uint32_t mode = 0;
    if (Memory::TryRead32(kStageGameMode, mode) && (mode == kTitle || mode == kTitleReplay)) {
        return true;
    }

    // The pause window lives for the entire course. Its active flag distinguishes the menu.
    constexpr uint32_t kPauseWindowInstance = 0x8042A33Cu;
    constexpr uint32_t kPauseWindowActive = 0x291u;
    uint32_t window = 0;
    return Memory::TryRead32(kPauseWindowInstance, window) &&
           window >= 0x80000000u && window < 0x81800000u - kPauseWindowActive &&
           Memory::Read8(window + kPauseWindowActive) != 0;
}
