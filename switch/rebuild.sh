#!/bin/bash
# Incremental rebuild: recompile the named translation units, relink, verify, package.
#
#   ./rebuild.sh [src/relative/unit.cpp ...] [--translated] [--compile-only | --pack]
#
# With no arguments it only relinks. Unit paths are relative to /src (the mounted working tree),
# e.g. switch/log_channel.cpp or runtime/src/main.cpp. The object name follows build-pie.sh:
# the path below runtime/src or aurora-main/lib with slashes replaced by underscores.
# Use --translated after changing memory or ISA helpers in the translated game.
# NSMBW_BUILD_JOBS controls the number of translated shards compiled in parallel.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
OBJ=/src/obj-pie
PACK=/pack
TID=${TID:-01004E53A1B50000}
NAME=${NAME:-NSMBW}
backend_flags=()
backend_archives=()
case "${NSMBW_SWITCH_BACKEND:-gles}" in
    gles)
        DAWN=/deps/dawn-src
        DAWN_BUILD="$DAWN/build-switch"
        aurora_backend_flags=(-DDAWN_ENABLE_BACKEND_OPENGL -DDAWN_ENABLE_BACKEND_OPENGLES)
        graphics_libs=(-lnx -lEGL -lGLESv2 -lglapi)
        graphics_tail=(-ldrm_nouveau -lnx)
        ;;
    deko3d)
        DAWN=/deko/dawn
        DAWN_BUILD=/deko/dawn-build
        backend_flags=(-DNSMBW_USE_DEKO3D)
        backend_archives=(/deko/uam/build-switch/libuam.a)
        aurora_backend_flags=(-DDAWN_ENABLE_BACKEND_DEKO3D)
        graphics_libs=(-ldeko3d -lnx)
        graphics_tail=(-lnx)
        ;;
    *) echo "Unknown Switch renderer: $NSMBW_SWITCH_BACKEND" >&2; exit 2 ;;
esac
SPECS=${SPECS:-/src/switch/switch-rw.specs}
if [ ! -f "$SPECS" ]; then
    echo "Missing linker specs: $SPECS. Run /src/switch/make-rw-ldscript.py first." >&2
    exit 1
fi

log_flags=()
if [ "${NSMBW_LOG_HOST+x}" = x ]; then
    log_flags+=("-DNSMBW_LOG_HOST=\"$NSMBW_LOG_HOST\"")
fi

INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/runtime/third_party/pugixml
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public -isystem /deps/absl
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN_BUILD/gen/include"
DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"

pack=0
compile_only=0
translated=0
units=()
for arg in "$@"; do
    case "$arg" in
        --pack) pack=1 ;;
        --compile-only) compile_only=1 ;;
        --translated) translated=1 ;;
        *) units+=("$arg") ;;
    esac
done
if [ "$compile_only" = 1 ] && [ "$pack" = 1 ]; then
    echo "--compile-only and --pack cannot be combined" >&2
    exit 2
fi

for unit in "${units[@]:-}"; do
    [ -z "$unit" ] && continue
    echo "== compile $unit =="
    case "$unit" in
        switch/*)
            base=$(basename "$unit" .cpp)
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 -D__SWITCH__ "${log_flags[@]}" "${backend_flags[@]}" -fPIE -c -O2 -fexceptions -frtti \
                -I/src/switch -isystem $DEVKITPRO/libnx/include -isystem /deps/SDL/include \
                -isystem /src/runtime/include "$unit" -o "$OBJ/$base.o"
            ;;
        runtime/src/*)
            rel=${unit#runtime/src/}
            base=$(echo "$rel" | sed 's|/|_|g').o
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 $DEFINES "${backend_flags[@]}" -fPIE -c -O2 -fexceptions -frtti \
                $INCLUDES "$unit" -o "$OBJ/$base"
            ;;
        aurora-main/*)
            rel=${unit#aurora-main/lib/}
            base="aur_${rel//\//_}.o"
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 $DEFINES "${backend_flags[@]}" -DAURORA_ENABLE_GX -DWEBGPU_DAWN \
                "${aurora_backend_flags[@]}" \
                -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$unit" -o "$OBJ/aurora/$base"
            ;;
        *) echo "unknown unit prefix: $unit" >&2; exit 2 ;;
    esac
done
if [ "$translated" = 1 ]; then
    echo "== compile translated game =="
    export CXX OBJ INCLUDES DEFINES
    printf '%s\0' /src/generated/build_shards/base_common/*.cpp |
        xargs -0 -r -P "${NSMBW_BUILD_JOBS:-4}" -n 1 bash -ec '
            unit="$1"
            base=$(basename "$unit" .cpp)
            echo "  $base"
            # shellcheck disable=SC2086
            "$CXX" -std=gnu++20 $DEFINES -fPIE -c -O2 $INCLUDES "$unit" -o "$OBJ/game/game_$base.o"
        ' _
fi
if [ "$compile_only" = 1 ]; then exit 0; fi

echo "== link =="
OBJS=$(ls "$OBJ"/*.o | grep -v "product_retro_rewind_product")
OBJS="$OBJS $(ls "$OBJ"/aurora/*.o "$OBJ"/gen/*.o "$OBJ"/game/*.o "$OBJ"/sys/*.o "$OBJ"/mbedtls/*.o)"
LIBS=$(ls "$OBJ"/lib/*.o | grep -vE "cryptopp_(test|bench|datatest|dlltest|validat|regtest)")
ABSL=$(ls "$OBJ"/absl/*.o | grep -vE "${ABSL_EXCLUDE:-print_hash_of|gentables|mock_log|status_matchers|vdso_support|elf_mem_image|stack_consumption|failure_signal_handler|examine_stack}")

wrap_flags=()
if [ -f "$OBJ/thread_tls.o" ]; then
    wrap_flags+=(-Wl,--wrap=pthread_create,--wrap=pthread_mutex_lock,--wrap=pthread_mutex_trylock
                 -Wl,--wrap=pthread_cond_wait,--wrap=pthread_cond_timedwait,--wrap=pthread_join,--wrap=pthread_detach)
fi
if [ -f "$OBJ/cxa_throw_probe.o" ]; then
    wrap_flags+=(-Wl,--wrap=__cxa_throw,--wrap=_ZSt9terminatev,--wrap=pthread_once,--wrap=_ZSt20__throw_system_errori)
fi

if ! "$CXX" -specs="$SPECS" -Wl,-z,nopack-relative-relocs -Wl,-z,notext \
    "${wrap_flags[@]}" \
    -o /src/wiicompiled.elf \
    $OBJS $LIBS $ABSL "$OBJ"/fmt/*.o \
    "$DAWN_BUILD"/src/dawn/native/libwebgpu_dawn.a "${backend_archives[@]}" /src/switch/execvp_stub.S \
    -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib "${graphics_libs[@]}" \
    ${RUNTIME_LIBS:--L$DEVKITPRO/devkitA64/aarch64-none-elf/lib -lstdc++ -lc -lm -lgcc -lsysbase} \
    "${graphics_tail[@]}" > /src/link-rebuild.log 2>&1; then
    tail -20 /src/link-rebuild.log >&2
    exit 1
fi
echo "link ok"

python3 /src/switch/patch_dynamic.py /src/wiicompiled.elf

python3 - "$DEVKITPRO/devkitA64/aarch64-none-elf/bin/readelf" <<'PY'
import re
import subprocess
import sys

headers, relocations = subprocess.check_output(
    [sys.argv[1], "-lW", "-rW", "/src/wiicompiled.elf"], text=True
).split("Relocation section '.rela.dyn'", 1)
writable = []
for line in headers.splitlines():
    fields = line.split()
    if fields and fields[0] == "LOAD" and "W" in "".join(fields[6:-1]):
        start = int(fields[2], 16)
        writable.append((start, start + int(fields[5], 16)))

targets = [int(address, 16) for address in re.findall(r"^\s*([\da-fA-F]+)\s+\S+\s+R_AARCH64_",
                                                      relocations.split("\n\n", 1)[0], re.M)]
if not targets or not writable:
    sys.exit("Missing dynamic relocations or writable load segments")
bad = sum(not any(start <= address < end for start, end in writable) for address in targets)
print(f"RELOCATIONS OUTSIDE WRITABLE SEGMENTS: {bad} of {len(targets)}")
if bad:
    sys.exit(1)
PY

"$DEVKITPRO/tools/bin/elf2nso" /src/wiicompiled.elf "$PACK/exefs/main" && echo "nso ok"

if [ "$pack" = "1" ]; then
    npdm_json=$(mktemp)
    trap 'rm -f "$npdm_json"' EXIT
    python3 - /src/switch/game.npdm.json "$TID" "$npdm_json" <<'PY'
import json
import sys
from pathlib import Path

source, title_id, output = sys.argv[1:]
config = json.loads(Path(source).read_text())
config["title_id"] = f"0x{int(title_id, 16):016x}"
Path(output).write_text(json.dumps(config))
PY
    "$DEVKITPRO/tools/bin/npdmtool" "$npdm_json" "$PACK/exefs/main.npdm"
    rm -rf "$PACK/out_rb" && mkdir -p "$PACK/out_rb"
    /deps/hacBrewPack/hacbrewpack --keyset "$PACK/keys.dat" --exefsdir "$PACK/exefs" \
        --controldir "$PACK/control_g" --romfsdir "$PACK/romfs" --nologo --plaintext \
        --titleid "$TID" --titlename "$NAME" --tempdir /tmp/hbprb --nspdir "$PACK/out_rb" 2>&1 | tail -1
    ls -la "$PACK/out_rb"/*.nsp | awk '{print $5" bytes"}'
fi
