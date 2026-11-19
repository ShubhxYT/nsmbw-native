#!/bin/bash
# Build a tiny probe NSP so runtime-level problems reproduce without the 64 MB game package.
#
#   ./build-probe.sh <name> <source.cpp> [defines...]
#
# STOCK=1 links devkitPro's untouched libraries instead of the re-PIC ones, to tell a port bug
# apart from a problem introduced by the .rodata -> .data.rel.ro rewrite.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
CXX="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++"
NAME=$1
SRC=$2
shift 2
PACK=/pack
TID=${TID:-01004E53A1B50000}
mkdir -p /src/probe

# shellcheck disable=SC2086
$CXX -std=gnu++20 -D__SWITCH__ -fPIE -c -O2 -fexceptions -frtti "$@" \
    -isystem $DEVKITPRO/libnx/include "$SRC" -o /src/probe/$NAME.o

if [ "${STOCK:-0}" = "1" ]; then
    LIBS=(-L$DEVKITPRO/devkitA64/aarch64-none-elf/lib -lstdc++ -lc -lm -lgcc
          -L$DEVKITPRO/devkitA64/lib/gcc/aarch64-none-elf/15.2.0)
    EXTRA=()
else
    LIBS=()
    EXTRA=(/src/piclibs/libstdc++_pic.a /src/piclibs/libc_pic.a /src/piclibs/libm_pic.a
           /src/piclibs/libgcc_pic.a /src/piclibs/libsysbase_pic.a)
fi

# shellcheck disable=SC2086
$CXX -specs=${SPECS:-$DEVKITPRO/libnx/switch.specs} -Wl,-z,nopack-relative-relocs -Wl,-z,notext \
    -o /src/probe/$NAME.elf /src/probe/$NAME.o ${EXTRA_OBJS:-} \
    -L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib -lnx \
    "${EXTRA[@]}" "${LIBS[@]}" 2>&1 | head -10

python3 /src/patch_dynamic.py /src/probe/$NAME.elf
$DEVKITPRO/tools/bin/elf2nso /src/probe/$NAME.elf $PACK/exefs/main
$DEVKITPRO/tools/bin/npdmtool $PACK/game_npdm.json $PACK/exefs/main.npdm 2>&1 | grep -v "Failed to get" || true

rm -rf $PACK/out_probe && mkdir -p $PACK/out_probe
/deps/hacBrewPack/hacbrewpack --keyset $PACK/keys.dat --exefsdir $PACK/exefs \
    --controldir $PACK/control_g --romfsdir $PACK/romfs --nologo --plaintext \
    --titleid "$TID" --titlename "$NAME" --tempdir /tmp/hbpp --nspdir $PACK/out_probe 2>&1 | tail -1
cp $PACK/out_probe/*.nsp /src/probe/$NAME.nsp
ls -la /src/probe/$NAME.nsp | awk '{print $5" bytes", $9}'
