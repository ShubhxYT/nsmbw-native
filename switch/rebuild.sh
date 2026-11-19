#!/bin/bash
# Incremental rebuild: recompile the named translation units, relink, verify, package.
#
#   ./rebuild.sh [src/relative/unit.cpp ...] [--pack]
#
# With no arguments it only relinks. Unit paths are relative to /src (the mounted working tree),
# e.g. switch/log_channel.cpp or runtime/src/main.cpp. The object name follows build-pie.sh:
# the path below runtime/src (or switch/) with slashes replaced by underscores.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
CC="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
DAWN=/deps/dawn-src
OBJ=/src/obj-pie
PACK=/pack
TID=${TID:-01004E53A1B50000}
NAME=${NAME:-NSMBW}

INCLUDES="-isystem $DEVKITPRO/libnx/include -isystem $DEVKITPRO/portlibs/switch/include
  -I/src/runtime/include -I/src/runtime/src -I/src
  -isystem /src/runtime/third_party -isystem /src/runtime/third_party/toml11
  -isystem /src/aurora-main/include -isystem /src/aurora-main/lib
  -isystem /deps/SDL/include -isystem /deps/imgui -isystem /deps/fmt/include
  -isystem /deps/xxhash -isystem /deps/tracy/public
  -isystem /deps/sqlite -isystem /deps/png -isystem /deps/zlib
  -isystem $DAWN/include -isystem $DAWN/build-switch/gen/include"
DEFINES="-D__SWITCH__ -DTARGET_PC -DCRYPTOPP_DISABLE_ASM"

pack=0
units=()
for arg in "$@"; do
    case "$arg" in
        --pack) pack=1 ;;
        *) units+=("$arg") ;;
    esac
done

for unit in "${units[@]:-}"; do
    [ -z "$unit" ] && continue
    echo "== compile $unit =="
    case "$unit" in
        switch/*)
            base=$(basename "$unit" .cpp)
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 -D__SWITCH__ -fPIE -c -O2 -fexceptions -frtti \
                -isystem $DEVKITPRO/libnx/include "$unit" -o "$OBJ/$base.o"
            ;;
        runtime/src/*)
            rel=${unit#runtime/src/}
            base=$(echo "$rel" | sed 's|/|_|g').o
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 $DEFINES -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$unit" -o "$OBJ/$base"
            ;;
        aurora-main/*)
            rel=${unit#aurora-main/lib/}
            base="aur_$(echo "$rel" | sed 's|/|_|g; s|\.cpp$|.o|')"
            # shellcheck disable=SC2086
            $CXX -std=gnu++20 $DEFINES -DAURORA_ENABLE_GX -DWEBGPU_DAWN \
                -DDAWN_ENABLE_BACKEND_OPENGL -DDAWN_ENABLE_BACKEND_OPENGLES \
                -fPIE -c -O2 -fexceptions -frtti $INCLUDES "$unit" -o "$OBJ/aurora/$base"
            ;;
        *) echo "unknown unit prefix: $unit" >&2; exit 2 ;;
    esac
done

echo "== link =="
OBJS=$(ls "$OBJ"/*.o | grep -v "product_retro_rewind_product")
OBJS="$OBJS $(ls "$OBJ"/aurora/*.o "$OBJ"/gen/*.o "$OBJ"/game/*.o "$OBJ"/sys/*.o "$OBJ"/mbedtls/*.o)"
LIBS=$(ls "$OBJ"/lib/*.o | grep -vE "cryptopp_(test|bench|datatest|dlltest|validat|regtest)")
ABSL=$(ls "$OBJ"/absl/*.o | grep -vE "${ABSL_EXCLUDE:-print_hash_of|gentables|mock_log|status_matchers|vdso_support|elf_mem_image|stack_consumption|failure_signal_handler|examine_stack}")

$CXX -specs=${SPECS:-$DEVKITPRO/libnx/switch.specs} -Wl,-z,nopack-relative-relocs -Wl,-z,notext \
    -o /src/wiicompiled.elf \
    $OBJS $LIBS $ABSL "$OBJ"/fmt/*.o \
    "$DAWN"/build-switch/src/dawn/native/libwebgpu_dawn.a /src/switch/execvp_stub.S \
    -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib -lnx -lEGL -lGLESv2 -lglapi \
    ${RUNTIME_LIBS:-/src/piclibs/libstdc++_pic.a /src/piclibs/libstdc++fs_pic.a /src/piclibs/libc_pic.a /src/piclibs/libm_pic.a /src/piclibs/libgcc_pic.a /src/piclibs/libsysbase_pic.a} \
    > /src/link-rebuild.log 2>&1
echo "link exit=$?"
if grep -qE "undefined reference|multiple definition" /src/link-rebuild.log; then
    echo "link failed"; grep -E "undefined reference|multiple definition" /src/link-rebuild.log | head -5
    exit 1
fi

python3 /src/patch_dynamic.py /src/wiicompiled.elf

python3 - <<'PY'
import re, struct, subprocess
elf = "/src/wiicompiled.elf"
d = open(elf, "rb").read()
out = subprocess.run(["/opt/devkitpro/devkitA64/aarch64-none-elf/bin/readelf", "-SW", elf],
                     capture_output=True, text=True).stdout
ro = rela = None
for line in out.splitlines():
    m = re.search(r"\.rodata\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+?\s+([0-9a-f]+)", line)
    if m and ro is None:
        ro = (int(m.group(1), 16), int(m.group(2), 16))
    m2 = re.search(r"\.rela\.dyn\s+RELA\s+[0-9a-f]+\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
    if m2 and rela is None:
        rela = (int(m2.group(1), 16), int(m2.group(2), 16))
count = rela[1] // 24
bad = 0
for i in range(count):
    off, _i, _a = struct.unpack_from("<QQq", d, rela[0] + i * 24)
    if ro[0] <= off < ro[0] + ro[1]:
        bad += 1
print(f"RODATA RELOCATIONS: {bad} of {count}")
PY

"$DEVKITPRO/tools/bin/elf2nso" /src/wiicompiled.elf "$PACK/exefs/main" && echo "nso ok"

if [ "$pack" = "1" ]; then
    rm -rf "$PACK/out_rb" && mkdir -p "$PACK/out_rb"
    /deps/hacBrewPack/hacbrewpack --keyset "$PACK/keys.dat" --exefsdir "$PACK/exefs" \
        --controldir "$PACK/control_g" --romfsdir "$PACK/romfs" --nologo --plaintext \
        --titleid "$TID" --titlename "$NAME" --tempdir /tmp/hbprb --nspdir "$PACK/out_rb" 2>&1 | tail -1
    ls -la "$PACK/out_rb"/*.nsp | awk '{print $5" bytes"}'
fi
