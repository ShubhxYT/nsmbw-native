#!/bin/bash
# Link the Switch runtime, game and renderer into an ELF that elf2nso accepts.
#
# The five invariants this encode (each was a real crash on hardware):
#   1. exactly three PT_LOAD segments - elf2nso refuses anything else and silently corrupts the NSO
#   2. .dynamic must end with DT_NULL - patched afterwards by patch_dynamic.py
#   3. no relocation may target .rodata - hence the *_pic.a archives, linked AFTER the system libs
#   4. RELA, not RELR - libnx's RELR walker is fragile for this image layout
#   5. the PIC archives come last, or libc's fcntl collides with libnx's
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
OBJ=${OBJ:-/src/obj-pie}
PIC=${PIC:-/src/piclibs}
DAWN=${DAWN:-/deps/dawn-src/build-switch/src/dawn/native/libwebgpu_dawn.a}

OBJS=$(ls "$OBJ"/*.o | grep -v "product_retro_rewind_product")
OBJS="$OBJS $(ls "$OBJ"/aurora/*.o "$OBJ"/gen/*.o "$OBJ"/game/*.o "$OBJ"/sys/*.o "$OBJ"/mbedtls/*.o)"
LIBS=$(ls "$OBJ"/lib/*.o | grep -vE "cryptopp_(test|bench|datatest|dlltest|validat|regtest)")
ABSL=$(ls "$OBJ"/absl/*.o | grep -vE "print_hash_of|gentables|mock_log|status_matchers|vdso_support|elf_mem_image|stack_consumption|failure_signal_handler|examine_stack")

$CXX -specs="$DEVKITPRO/libnx/switch.specs" -Wl,-z,nopack-relative-relocs -Wl,-z,notext \
    -o /src/wiicompiled.elf \
    $OBJS $LIBS $ABSL "$OBJ"/fmt/*.o "$DAWN" /src/switch/execvp_stub.S \
    -L"$DEVKITPRO/libnx/lib" -L"$DEVKITPRO/portlibs/switch/lib" -lnx -lEGL -lGLESv2 -lglapi \
    "$PIC/libstdc++_pic.a" "$PIC/libstdc++fs_pic.a" "$PIC/libc_pic.a" "$PIC/libm_pic.a" \
    "$PIC/libgcc_pic.a" "$PIC/libsysbase_pic.a"

python3 /src/patch_dynamic.py /src/wiicompiled.elf

# Report the number that must be zero.
python3 - <<'PY'
import re, struct, subprocess
elf = "/src/wiicompiled.elf"
d = open(elf, "rb").read()
out = subprocess.run(["/opt/devkitpro/devkitA64/aarch64-none-elf/bin/readelf", "-SW", elf],
                     capture_output=True, text=True).stdout
ro = rela = None
for line in out.splitlines():
    m = re.search(r"\.rodata\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", line)
    if m and ro is None:
        ro = (int(m.group(1), 16), int(m.group(2), 16))
    m2 = re.search(r"\.rela\.dyn\s+RELA\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
    if m2 and rela is None:
        rela = (int(m2.group(2), 16), int(m2.group(3), 16))
count = rela[1] // 24
bad = 0
for i in range(count):
    off, _info, _add = struct.unpack_from("<QQq", d, rela[0] + i * 24)
    if ro[0] <= off < ro[0] + ro[1]:
        bad += 1
print(f"RODATA RELOCATIONS: {bad} of {count}")
PY
