#!/bin/bash
# Package the linked ELF as an installable NSP.
#
# Title ID must be a *base application*: 0100 + non-zero groups + trailing 0000. Anything else is
# classified as an update or addon ("Game uses titleID from update!", "Creating game… Failed 0x2e4").
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
PACK=${PACK:-/tmp/nsmbw-pack}
TID=${TID:-01004E53A1B50000}
NAME=${NAME:-NSMBW}
OUT=${OUT:-$PACK/out}

"$DEVKITPRO/tools/bin/elf2nso" /src/wiicompiled.elf "$PACK/exefs/main"
"$DEVKITPRO/tools/bin/npdmtool" "$PACK/npdm.json" "$PACK/exefs/main.npdm" 2>&1 | grep -v "Failed to get" || true

rm -rf "$OUT" && mkdir -p "$OUT"
/deps/hacBrewPack/hacbrewpack --keyset "$PACK/keys.dat" --exefsdir "$PACK/exefs" \
    --controldir "$PACK/control_g" --romfsdir "$PACK/romfs" --nologo --plaintext \
    --titleid "$TID" --titlename "$NAME" --tempdir /tmp/hbp --nspdir "$OUT" 2>&1 | tail -2

ls -la "$OUT"/*.nsp
