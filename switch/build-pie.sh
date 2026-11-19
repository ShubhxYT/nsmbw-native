#!/bin/bash
# Rebuild every object with -fPIE and link with libnx's stock specs.
#
# Why: switch.specs links with -pie and -z text, which rejects any object that is not
# position-independent. Building with -fPIE satisfies that, and the resulting image needs no
# load-time relocation — which is required, because a title installed from an NSP (and hbmenu) both
# load the NSO at its link address without applying relocations. The earlier build worked around the
# rejection with -z notext and produced an image with 39,692 RELR relocations: it crashed on launch.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
D="$DEVKITPRO/devkitA64/aarch64-none-elf"
DAWN=/deps/dawn-src
OBJ=/src/obj-pie

INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include"

DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"
AURORA_DEFINES="-DAURORA_ENABLE_GX -DWEBGPU_DAWN -DDAWN_ENABLE_BACKEND_OPENGL -DDAWN_ENABLE_BACKEND_OPENGLES"
PIC="-fPIE -pie-independent-not-a-flag"

rm -rf "$OBJ"
mkdir -p "$OBJ"/{aurora,game,gen,sys,mbedtls,lib,absl,fmt}

echo "== runtime =="
for f in $(find /src/runtime/src -name '*.cpp'); do
  b=$(echo "$f" | sed 's|/src/runtime/src/||; s|/|_|g').o
  case "$b" in discord_presence*|guest_flat_memory_macos*|hd_texture_pack*) continue ;; esac
  $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "$OBJ/$b" || echo "  skip $b"
done

echo "== generated =="
$CC -fPIE -c /src/generated/data_sections_init_blobs.S -o "$OBJ/gen/data_blobs.o"
for f in /src/generated/*.cpp; do
  $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "$OBJ/gen/gen_$(basename "$f" .cpp).o"
done

echo "== libraries =="
for f in /src/runtime/third_party/cryptopp/*.cpp; do
  b=cryptopp_$(basename "$f" .cpp).o
  case "$b" in *test*|*bench*|*datatest*|*dlltest*|*validat*|*regtest*) continue ;; esac
  $CXX -std=gnu++17 -DCRYPTOPP_DISABLE_ASM -DCRYPTOPP_NO_GLOBAL_BYTE -fPIE -c -O2 -I/src/runtime/third_party "$f" -o "$OBJ/lib/$b"
done
$CXX -std=gnu++17 -fPIE -c -O2 -I/src/runtime/third_party/pugixml /src/runtime/third_party/pugixml/pugixml.cpp -o "$OBJ/lib/pugixml.o"
$CC -fPIE -c -O2 -I/deps/xxhash /deps/xxhash/xxhash.c -o "$OBJ/lib/xxhash.o"
for f in imgui imgui_draw imgui_tables imgui_widgets; do
  $CXX -std=gnu++17 -fPIE -c -O2 '-DIMGUI_USER_CONFIG="aurora/imgui_config.h"' -I/deps/imgui -I/src/aurora-main/include "/deps/imgui/$f.cpp" -o "$OBJ/lib/$f.o"
done
for spec in "imgui_impl_sdl3 imgui_impl_sdl3.cpp" "imgui_impl_sdlrenderer3 imgui_impl_sdlrenderer3.cpp" "imgui_impl_wgpu imgui_impl_wgpu.cpp"; do
  set -- $spec
  $CXX -std=gnu++17 -fPIE -DIMGUI_IMPL_WEBGPU_BACKEND_DAWN '-DIMGUI_USER_CONFIG="aurora/imgui_config.h"' -c -O2 \
    -I/deps/imgui -I/deps/imgui/backends -I/src/aurora-main/include -isystem /deps/SDL/include \
    -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include -isystem $DEVKITPRO/libnx/include \
    "/deps/imgui/backends/$2" -o "$OBJ/lib/$1.o"
done
for f in format os; do
  $CXX -std=gnu++17 -fPIE -c -O2 -I/deps/fmt/include "/deps/fmt/src/$f.cc" -o "$OBJ/fmt/$f.o"
done

echo "== abseil =="
for f in $(find /deps/absl/absl -name '*.cc' | grep -vE 'test|benchmark'); do
  b=absl_$(echo "$f" | sed 's|/deps/absl/||; s|/|_|g').o
  $CXX -std=gnu++17 -D__SWITCH__ -fPIE -c -O2 -fexceptions -frtti -I/deps/absl \
    -isystem /src/switch/include -include /src/switch/include/switch_posix.h "$f" -o "$OBJ/absl/$b" 2>/dev/null || true
done

echo "== aurora =="
for f in /src/aurora-main/lib/*.cpp /src/aurora-main/lib/gfx/*.cpp /src/aurora-main/lib/gx/*.cpp \
         /src/aurora-main/lib/card/*.cpp /src/aurora-main/lib/webgpu/*.cpp /src/aurora-main/lib/dawn/*.cpp \
         /src/aurora-main/lib/dolphin/*/*.cpp; do
  case "$f" in */main.cpp|*/dvd/dvd.cpp|*/metalfx.mm) continue ;; esac
  b=aur_$(echo "$f" | sed 's|/src/aurora-main/lib/||; s|/|_|g').o
  $CXX -std=gnu++20 $DEFINES $AURORA_DEFINES -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "$OBJ/aurora/$b" 2>/dev/null || echo "  skip $b"
done

echo "== translated game =="
for f in /src/generated/build_shards/base_common/*.cpp; do
  $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 $INCLUDES "$f" -o "$OBJ/game/game_$(basename "$f" .cpp).o"
done

echo "== platform layer =="
$CXX -std=gnu++20 -D__SWITCH__ -fPIE -c -O2 -fexceptions -frtti -isystem $DEVKITPRO/libnx/include \
  -isystem /deps/SDL/include /src/switch/sdl_switch.cpp -o "$OBJ/sdl_switch.o"
$CXX -std=gnu++17 -fPIE -c -O2 -isystem /src/switch/include -isystem $DEVKITPRO/libnx/include \
  /src/switch/absl_horizon.cpp -o "$OBJ/absl_horizon.o"
$CXX -std=gnu++17 -fPIE -c -O2 -isystem /src/switch/include -isystem $DEVKITPRO/libnx/include \
  /src/switch/switch_shims.cpp -o "$OBJ/switch_shims.o"
$CXX -std=gnu++20 -D__SWITCH__ -fPIE -c -O2 -fexceptions -frtti -isystem $DEVKITPRO/libnx/include \
  /src/switch/log_channel.cpp -o "$OBJ/log_channel.o"
$CXX -std=gnu++20 -D__SWITCH__ -DTARGET_PC -fPIE -c -O2 -fexceptions -frtti -isystem $DEVKITPRO/libnx/include \
  -I/src/runtime/include /src/switch/discord_stub.cpp -o "$OBJ/discord_stub.o"
$CXX -std=gnu++20 -D__SWITCH__ -DTARGET_PC -fPIE -c -O2 -fexceptions -frtti -isystem $DEVKITPRO/libnx/include \
  -I/src/runtime/include /src/switch/hd_texture_stub.cpp -o "$OBJ/hd_texture_stub.o"
$CC -fPIE -c /src/runtime/src/platform/android/co_switch.S -o "$OBJ/co_switch.o"

echo "== mbedtls =="
for f in /deps/mbedtls/library/*.c; do
  b=$(basename "$f" .c).o
  if ! $CC -fPIE -c -O2 -D__unix__ -DMBEDTLS_PLATFORM_MS_TIME_ALT -I/deps/mbedtls/include -I/deps/mbedtls/library \
      -isystem $DEVKITPRO/libnx/include "$f" -o "$OBJ/mbedtls/$b" 2>/dev/null; then
    echo "  retry $b without __unix__"
    $CC -fPIE -c -O2 -DMBEDTLS_PLATFORM_MS_TIME_ALT -I/deps/mbedtls/include -I/deps/mbedtls/library \
      -isystem $DEVKITPRO/libnx/include "$f" -o "$OBJ/mbedtls/$b" 2>/dev/null || echo "  skip $b"
  fi
done

echo "== link =="
OBJS=$(ls "$OBJ"/*.o "$OBJ"/aurora/*.o "$OBJ"/gen/*.o "$OBJ"/game/*.o "$OBJ"/sys/*.o "$OBJ"/mbedtls/*.o)
LIBS=$(ls "$OBJ"/lib/*.o | grep -vE 'cryptopp_(test|bench|datatest|dlltest|validat|regtest)')
ABSL=$(ls "$OBJ"/absl/*.o | grep -vE 'print_hash_of|gentables|mock_log|status_matchers|vdso_support|elf_mem_image|stack_consumption|failure_signal_handler|examine_stack')
$CXX -specs=$DEVKITPRO/libnx/switch.specs -o /src/wiicompiled.elf \
  $OBJS $LIBS $ABSL "$OBJ"/fmt/*.o \
  $DAWN/build-switch/src/dawn/native/libwebgpu_dawn.a \
  /src/switch/execvp_stub.S \
  -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib \
  -lnx -lEGL -lGLESv2 -lglapi

echo "== verify no load-time relocations =="
RE=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-readelf
$RE -d /src/wiicompiled.elf | grep -iE "relr|rela|dynamic section" || echo "  no dynamic relocation section"
$RE -r /src/wiicompiled.elf 2>/dev/null | head -3 || true

$DEVKITPRO/tools/bin/elf2nso /src/wiicompiled.elf /src/wiicompiled.nso
ls -la /src/wiicompiled.elf /src/wiicompiled.nso
