#pragma once

#define NSMBW_SWITCH_DATA_ROOT "sdmc:/switch/aaa-nsmbw"
#define NSMBW_SWITCH_BUNDLED_GAME_ROOT "romfs:/game"
#define NSMBW_SWITCH_BUNDLED_NAND_SEED "romfs:/wii_bootstrap"

#ifdef __cplusplus
extern "C" bool switch_has_bundled_game();
#endif
