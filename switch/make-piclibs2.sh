#!/bin/bash
# Derive PIC versions of devkitPro's prebuilt libraries.
#
# Why this exists: an NSP title is loaded with relocations, and a relocation whose target lives in
# .rodata fails, because the NSO loader maps that section read-only. Atmosphère reports the resulting
# permission fault as "Fault Address 0".
#
# The rewrite must be surgical. Renaming every .rodata* section in libc breaks newlib's locale
# tables: printf then drops every conversion (%d, %s, %c all print nothing), setlocale/newlocale
# return NULL, and libstdc++'s iostream global initialiser dies. Only sections that a relocation
# actually targets need to become writable, so this renames exactly the section named by each
# .rela.<section> entry and nothing else.
#
# The section name is extracted by pattern: readelf right-aligns the section index, so the name is
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
    local renamed=0
    for o in *.o; do
        # Every .rela.<name> means the object relocates the section <name>.
        local targets
        targets=$($READELF -SW "$o" 2>/dev/null | grep -oE '\.rela\.[^ ]+' |
                  sed 's/^\.rela//' | sort -u |
                  grep -E '^\.rodata' || true)
        if [ -z "$targets" ]; then
            cp "$o" "$OUT/$libname-$o"
            continue
        fi
        local args=""
        for sec in $targets; do
            args="$args --rename-section $sec=.data.rel.ro${sec#.rodata},alloc,load,data"
            renamed=$((renamed + 1))
        done
        # shellcheck disable=SC2086
        $OBJCOPY $args "$o" "$OUT/$libname-$o"
    done
    echo "  renamed $renamed read-only relocation targets"
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
