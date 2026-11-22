#pragma once

#define NSMBW_SWITCH_DATA_ROOT "sdmc:/switch/aaa-nsmbw"
#define NSMBW_SWITCH_BUNDLED_GAME_ROOT "romfs:/game"
#define NSMBW_SWITCH_BUNDLED_NAND_SEED "romfs:/wii_bootstrap"
#define NSMBW_SWITCH_SAVE_DEVICE "nsmbwsave"
#define NSMBW_SWITCH_NATIVE_NAND_ROOT NSMBW_SWITCH_SAVE_DEVICE ":/NAND"

#ifdef __cplusplus
extern "C" bool switch_has_bundled_game();
extern "C" bool switch_mount_profile_save(const char* legacyNandRoot);
extern "C" bool switch_commit_profile_save();
#endif
