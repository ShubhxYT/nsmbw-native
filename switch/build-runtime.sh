#!/bin/bash
# Build the Switch binary: runtime + libraries + translated game + Dawn, linked for aarch64-none-elf.
#
# Run inside the devkitPro container, with these mounts:
#   /src   = this checkout (or a working copy of it)
#   /deps  = imgui, fmt, absl, xxhash, sqlite, png, zlib, tracy, SDL3, dawn-src (+ build-switch)
#
# Flags that matter, each learned the hard way:
#   -D__SWITCH__ -DTARGET_PC            Aurora only provides u32/BOOL on the TARGET_PC path
#   -fexceptions -frtti                 the runtime uses exceptions; devkitPro's template disables them
#   -DCRYPTOPP_DISABLE_ASM              Crypto++ assembly is x86/ARM32 only
#   -DAURORA_ENABLE_GX -DWEBGPU_DAWN    without these Aurora compiles without its renderer at all
#   -DDAWN_ENABLE_BACKEND_OPENGL(ES)    must match how Dawn itself was configured
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
D="$DEVKITPRO/devkitA64/aarch64-none-elf"
DAWN=/deps/dawn-src

# Dawn's generated headers must match the Dawn being linked; the copy in the iOS kit is a different
# version and mixing them produces silent ABI mismatches.
INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include
  -isystem /src/switch/include -include /src/switch/include/switch_posix.h"

DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"
AURORA_DEFINES="-DAURORA_ENABLE_GX -DWEBGPU_DAWN -DDAWN_ENABLE_BACKEND_OPENGL -DDAWN_ENABLE_BACKEND_OPENGLES"

mkdir -p /src/obj /src/obj/lib /src/obj/aurora /src/obj/absl /src/obj/fmt /src/obj/game /src/obj/sys /src/obj/gen

echo "== runtime =="
for f in $(find /src/runtime/src -name '*.cpp'); do
  b=$(echo "$f" | sed 's|/src/runtime/src/||; s|/|_|g').o
  case "$b" in
    discord_presence*|guest_flat_memory_macos*|hd_texture_pack*) continue ;;  # platform exclusions
  esac
  $CXX -std=gnu++20 $DEFINES -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "/src/obj/$b" 2>/dev/null || echo "  skip $b"
done

echo "== generated (guest symbol table, data sections) =="
$CC -c /src/generated/data_sections_init_blobs.S -o /src/obj/gen/data_blobs.o
for f in /src/generated/*.cpp; do
  $CXX -std=gnu++20 $DEFINES -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "/src/obj/gen/gen_$(basename "$f" .cpp).o"
done

echo "== libraries =="
for f in /src/runtime/third_party/cryptopp/*.cpp; do
  b=cryptopp_$(basename "$f" .cpp).o
  case "$b" in *test*|*bench*|*datatest*|*dlltest*|*validat*|*regtest*) continue ;; esac
  $CXX -std=gnu++17 -DCRYPTOPP_DISABLE_ASM -DCRYPTOPP_NO_GLOBAL_BYTE -c -O2 -I/src/runtime/third_party "$f" -o "/src/obj/lib/$b"
done
$CXX -std=gnu++17 -c -O2 -I/src/runtime/third_party/pugixml /src/runtime/third_party/pugixml/pugixml.cpp -o /src/obj/lib/pugixml.o
$CC -c -O2 -I/deps/xxhash /deps/xxhash/xxhash.c -o /src/obj/lib/xxhash.o
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  $CXX -std=gnu++17 -c -O2 -I/deps/imgui "/deps/imgui/$f.cpp" -o "/src/obj/lib/$f.o"
done
for f in format os; do
  $CXX -std=gnu++17 -c -O2 -I/deps/fmt/include "/deps/fmt/src/$f.cc" -o "/src/obj/fmt/$f.o"
done

echo "== abseil =="
for f in $(find /deps/absl/absl -name '*.cc' | grep -vE 'test|benchmark'); do
  b=absl_$(echo "$f" | sed 's|/deps/absl/||; s|/|_|g').o
  $CXX -std=gnu++17 -D__SWITCH__ -c -O2 -fexceptions -frtti -I/deps/absl -isystem /src/switch/include \
    -include /src/switch/include/switch_posix.h "$f" -o "/src/obj/absl/$b" 2>/dev/null || true
done

echo "== aurora =="
for f in /src/aurora-main/lib/*.cpp /src/aurora-main/lib/gfx/*.cpp /src/aurora-main/lib/gx/*.cpp \
         /src/aurora-main/lib/card/*.cpp /src/aurora-main/lib/webgpu/*.cpp /src/aurora-main/lib/dawn/*.cpp \
         /src/aurora-main/lib/dolphin/*/*.cpp; do
  case "$f" in */main.cpp|*/dvd/dvd.cpp|*/metalfx.mm) continue ;; esac
  b=aur_$(echo "$f" | sed 's|/src/aurora-main/lib/||; s|/|_|g').o
  $CXX -std=gnu++20 $DEFINES $AURORA_DEFINES -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "/src/obj/aurora/$b" 2>/dev/null || true
done

echo "== translated game =="
for f in /src/generated/build_shards/base_common/*.cpp; do
  $CXX -std=gnu++20 $DEFINES -c -O2 $INCLUDES "$f" -o "/src/obj/game/game_$(basename "$f" .cpp).o"
done

echo "== platform layer and shims =="
$CXX -std=gnu++20 -D__SWITCH__ -c -O2 -fexceptions -frtti -isystem $DEVKITPRO/libnx/include \
  -isystem /deps/SDL/include /src/switch/sdl_switch.cpp -o /src/obj/sdl_switch.o
$CXX -std=gnu++17 -c -O2 -isystem /src/switch/include -isystem $DEVKITPRO/libnx/include \
  /src/switch/absl_horizon.cpp -o /src/obj/absl_horizon.o
$CXX -std=gnu++17 -c -O2 -isystem /src/switch/include -isystem $DEVKITPRO/libnx/include \
  /src/switch/switch_shims.cpp -o /src/obj/switch_shims.o

echo "== link =="
OBJS=$(ls /src/obj/*.o /src/obj/aurora/*.o /src/obj/gen/*.o /src/obj/game/*.o /src/obj/sys/*.o)
LIBS=$(ls /src/obj/lib/*.o | grep -vE 'cryptopp_(test|bench|datatest|dlltest|validat|regtest)')
ABSL=$(ls /src/obj/absl/*.o | grep -vE 'print_hash_of|gentables|mock_log|status_matchers|time_zone_libc|time_zone_format|vdso_support|elf_mem_image|stack_consumption|failure_signal_handler|examine_stack')
$CXX -specs=$DEVKITPRO/libnx/switch.specs -o /src/wiicompiled.elf \
  $OBJS $LIBS $ABSL /src/obj/fmt/*.o \
  $DAWN/build-switch/libwebgpu_dawn.a \
  /src/switch/execvp_stub.S \
  -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib \
  -lnx -lEGL -lGLESv2 -lglapi

echo "== nro =="
$DEVKITPRO/tools/bin/elf2nro /src/wiicompiled.elf /src/wiicompiled.nro \
  --nacp=/src/switch/nsmbw.nacp --icon=/src/switch/icon.jpg 2>/dev/null ||
  $DEVKITPRO/tools/bin/elf2nro /src/wiicompiled.elf /src/wiicompiled.nro
ls -la /src/wiicompiled.elf /src/wiicompiled.nro
