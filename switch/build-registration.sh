#!/bin/bash
# Compile the translator's bulk registration shards into obj-pie/gen/.
#
# build-pie.sh only globs generated/build_shards/base_common/, so the registration tables were never
# compiled and TranslatedFunctionRegistry ended up with 396 entries instead of the full function map -
# which is why ResolveEntry() could not find 0x80004050. Everything that links globs obj-pie/gen/*.o,
# so compiling them there is enough.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
DAWN=/deps/dawn-src
OBJ=/src/obj-pie
JOBS=$(nproc)

INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include"
DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"

compile_one() {
    local f=$1
    local out="$OBJ/gen/reg_$(basename "$f" .cpp).o"
    # shellcheck disable=SC2086
    $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 $INCLUDES "$f" -o "$out"
    echo "  $(basename "$out")"
}
export -f compile_one
export CXX INCLUDES DEFINES OBJ

mkdir -p "$OBJ/gen"
ls /src/generated/build_shards/base_registration/*.cpp |
    xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}

echo "compiled $(ls "$OBJ/gen"/reg_*.o | wc -l) registration shards"
