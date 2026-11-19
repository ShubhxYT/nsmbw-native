#!/bin/bash
# Derive PIC versions of devkitPro's prebuilt libraries.
#
# devkitPro builds its libraries non-PIC because homebrew NROs load at a fixed base. An NSP title is
# loaded with ASLR and needs relocations, and a relocation whose target lives in .rodata fails: the NSO
# loader maps that section read-only, and Atmosphère reports the resulting permission fault as
# "Fault Address 0". Every such relocation has to move to a writable section, so each .rodata* section
# in these archives is renamed to .data.rel.ro*.
#
# The section name must be extracted by pattern: readelf right-aligns the section index, so the name is
# field $2 for some objects and $3 for others.
set -euo pipefail

B=/opt/devkitpro/devkitA64/aarch64-none-elf/bin
D=/opt/devkitpro/devkitA64/aarch64-none-elf/lib
GD=/opt/devkitpro/devkitA64/lib/gcc/aarch64-none-elf
OUT=${1:-/src/piclibs}

AR=$B/ar
OBJCOPY=$B/objcopy
READELF=$B/readelf

pic() {
    local libname=$1 lib=$2
    echo "== $libname =="
    rm -rf /tmp/px && mkdir -p /tmp/px && cd /tmp/px
    $AR x "$lib"
    for o in *.o; do
        local rosecs
        rosecs=$($READELF -SW "$o" 2>/dev/null | grep -oE "\.rodata[^ ]*" | sort -u)
        if [ -z "$rosecs" ]; then
            cp "$o" "$OUT/$libname-$o"
            continue
        fi
        local args=""
        for sec in $rosecs; do
            # alloc + load keep the contents, data makes it writable.
            args="$args --rename-section $sec=.data.rel.ro${sec#.rodata},alloc,load,data"
        done
        $OBJCOPY $args "$o" "$OUT/$libname-$o"
    done
    $AR rcs "$OUT/${libname}_pic.a" "$OUT"/$libname-*.o
}

mkdir -p "$OUT"
pic libstdc++   $D/libstdc++.a
pic libstdc++fs $D/libstdc++fs.a
pic libc        $D/libc.a
pic libm        $D/libm.a
pic libsysbase  $D/libsysbase.a          # dotab_stdnull lives here (iosupport.o)
pic libgcc      "$(ls $GD/*/libgcc.a | head -1)"

ls -la "$OUT"/*.a
