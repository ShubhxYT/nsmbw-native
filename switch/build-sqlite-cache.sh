#!/bin/bash
# Build the SQLite amalgamation and native SD VFS into the deko3d object tree.
set -euo pipefail

DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
cc="$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc"
sqlite=/deps/sqlite
objects=/src/obj-pie/sys
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT

"$cc" -std=gnu11 -O2 -fPIE -DSQLITE_OS_OTHER=1 -DSQLITE_TEMP_STORE=3 \
    -DSQLITE_OMIT_LOAD_EXTENSION -c "$sqlite/sqlite3.c" -o "$staging/sqlite3.o"
"$cc" -std=gnu11 -O2 -fPIE -isystem "$DEVKITPRO/libnx/include" -I"$sqlite" \
    -c "$sqlite/nx-vfs.c" -o "$staging/sqlite_nx_vfs.o"
mv "$staging/sqlite3.o" "$staging/sqlite_nx_vfs.o" "$objects/"
