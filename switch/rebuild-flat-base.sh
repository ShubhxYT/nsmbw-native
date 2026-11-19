#!/bin/bash
# Recompile every translation unit that can inline MKW_FLAT_GUEST_BASE.
#
# MKW_FLAT_GUEST_BASE became a runtime value under __SWITCH__ (Horizon cannot place the arena at the
# fixed 256 GiB base the generated code used), so every object that touches guest memory must be
# rebuilt or it will keep computing addresses against a base that is not mapped.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
DAWN=/deps/dawn-src
OBJ=/src/obj-pie
JOBS=${JOBS:-$(nproc)}

INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include"
DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"

compile_runtime() {
    local f=$1
    local b
    b=$(echo "${f#/src/runtime/src/}" | sed 's|/|_|g').o
    case "$b" in discord_presence*|guest_flat_memory_macos*|hd_texture_pack*) return 0 ;; esac
    # shellcheck disable=SC2086
    $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$f" -o "$OBJ/$b" 2>/dev/null ||
        echo "  skip $b"
}
compile_shard() {
    local f=$1
    # shellcheck disable=SC2086
    $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 $INCLUDES "$f" \
        -o "$OBJ/game/game_$(basename "$f" .cpp).o"
}
compile_reg() {
    local f=$1
    # shellcheck disable=SC2086
    $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 $INCLUDES "$f" \
        -o "$OBJ/gen/reg_$(basename "$f" .cpp).o"
}

export -f compile_runtime compile_shard compile_reg
export CXX CC INCLUDES DEFINES OBJ

echo "== runtime =="
find /src/runtime/src -name '*.cpp' -print0 | xargs -0 -P "$JOBS" -I{} bash -c 'compile_runtime "$@"' _ {}

echo "== translated game shards =="
ls /src/generated/build_shards/base_common/*.cpp |
    xargs -P "$JOBS" -I{} bash -c 'compile_shard "$@"' _ {}

echo "== registration shards =="
ls /src/generated/build_shards/base_registration/*.cpp |
    xargs -P "$JOBS" -I{} bash -c 'compile_reg "$@"' _ {}

echo "done: $(ls "$OBJ"/*.o | wc -l) runtime, $(ls "$OBJ"/game/*.o | wc -l) shards, $(ls "$OBJ"/gen/*.o | wc -l) gen"
