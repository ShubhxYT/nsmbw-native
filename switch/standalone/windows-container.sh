#!/usr/bin/env bash
# Run the Windows builder's compilation and offline packaging in separate containers.
set -euo pipefail

tar -xf /input/recipe.tar -C /recipe
root=/recipe
out=/work
jobs=${NSMBW_BUILD_JOBS:-4}
receipt="$out/.build-inputs"
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }

mount_build_paths() {
    # The native linker and dependency build use these same paths on every host.
    ln -s "$source_dir" /src
    ln -s "$out/deps" /deps
    ln -s "$out/deko" /deko
}

case "$1" in
    compile)
        mkdir -p "$out"/{logs,tools,deps,deko}
        rm -f "$receipt"
        case "$(uname -m)" in
            x86_64) nod_asset=linux-x86_64 ;;
            aarch64) nod_asset=linux-aarch64 ;;
            *) die 'unsupported Linux container architecture' ;;
        esac
        disc=
        disc_dir=
        input="$out/disc-input"
        stamp="$out/.disc-input-id"
        if [[ -e /input/disc ]]; then
            rm -f "$stamp"
            rm -rf "$input"
            mv /input/disc "$input"
            printf '%s\n' "$3" >"$stamp"
        fi
        if [[ ! -f "$stamp" || "$(cat "$stamp")" != "$3" || ! -e "$input" ]]; then
            die 'staged disc input is missing or does not match the requested disc'
        fi
        case "$2" in
            disc) disc="$input" ;;
            disc-dir) disc_dir="$input" ;;
            *) die 'invalid disc input' ;;
        esac
        source "$root/switch/standalone/prepare.sh"
        prepare_switch_source
        mount_build_paths
        cd /src
        bash "$root/switch/standalone/build.sh" compile 2>&1 | tee "$out/logs/switch-build.log"
        printf '%s\n%s\n' "$source_dir" "$disc_dir" >"$receipt"
        ;;
    pack)
        mapfile -t build_inputs <"$receipt"
        source_dir=${build_inputs[0]}
        disc_dir=${build_inputs[1]}
        mount_build_paths
        ln -s "$disc_dir" /disc
        ln -s /input/keys /run/nsmbw-keys
        if [[ -f /input/icon ]]; then ln -s /input/icon /run/nsmbw-icon; fi
        cd /src
        bash "$root/switch/standalone/build.sh" pack "$2" 2>&1 | tee "$out/logs/pack.log"
        ;;
    *) die 'invalid build stage' ;;
esac
