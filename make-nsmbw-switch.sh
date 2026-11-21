#!/usr/bin/env bash
# Build a standalone Switch NSP from a local SMNE01 revision 1 disc.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
upstream=e5fce0f26a2229b8ed70357d2d503780235c0eba
version=1.1.3
disc=
disc_dir=
keys="${NSMBW_SWITCH_KEYS:-$HOME/.switch/prod.keys}"
out="$root/build/switch"
icon=
jobs=${NSMBW_BUILD_JOBS:-4}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
step() { printf '\n==> %s\n' "$*"; }
usage() {
    cat <<'HELP'
Usage: ./make-nsmbw-switch.sh --disc PATH [options]

  --disc PATH       Your SMNE01 revision 1 disc image (.rvz, .iso, .wbfs, .gcz, .wia)
  --disc-dir PATH   Use an already extracted disc (sys/ and files/)
  --keys PATH       Local Switch key file (default: ~/.switch/prod.keys)
  --out DIR         Build and output directory (default: build/switch)
  --version X.Y.Z   Display version and output filename (default: 1.1.3)
  --icon PATH       Optional PNG or JPEG HOME icon
  --jobs N          Parallel compile jobs (default: 4)
HELP
}

while (($#)); do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --disc|--disc-dir|--keys|--out|--version|--icon|--jobs)
            [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || die "$1 needs a value"
            case "$1" in
                --disc) disc=$2 ;;
                --disc-dir) disc_dir=$2 ;;
                --keys) keys=$2 ;;
                --out) out=$2 ;;
                --version) version=$2 ;;
                --icon) icon=$2 ;;
                --jobs) jobs=$2 ;;
            esac
            shift 2 ;;
        *) die "unknown option $1 (see --help)" ;;
    esac
done

[[ -n "$disc" && -z "$disc_dir" || -z "$disc" && -n "$disc_dir" ]] ||
    die 'give exactly one of --disc or --disc-dir'
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && ${#version} -le 15 ]] ||
    die 'version must be X.Y.Z and fit in 15 characters'
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die 'jobs must be a positive integer'
[[ -z "$disc" || -f "$disc" ]] || die "disc image not found: $disc"
[[ -z "$disc_dir" || -f "$disc_dir/sys/boot.bin" ]] || die "extracted disc not found: $disc_dir"
[[ -z "$icon" || -f "$icon" ]] || die "icon not found: $icon"
[[ -f "$keys" && -r "$keys" ]] || die 'a local Switch key file is required (use --keys or ~/.switch/prod.keys)'
for tool in git curl python3 dotnet docker; do
    command -v "$tool" >/dev/null || die "missing required command: $tool"
done
[[ "$(dotnet --list-sdks)" =~ (^|[[:space:]])8\. ]] || die 'install the .NET 8 SDK'
docker info >/dev/null 2>&1 || die 'start Docker before running the build'

case "$(uname -s):$(uname -m)" in
    Darwin:arm64) nod_asset=macos-arm64; platform=linux/arm64 ;;
    Darwin:x86_64) nod_asset=macos-x86_64; platform=linux/amd64 ;;
    Linux:aarch64|Linux:arm64) nod_asset=linux-aarch64; platform=linux/arm64 ;;
    Linux:x86_64) nod_asset=linux-x86_64; platform=linux/amd64 ;;
    *) die 'this build supports macOS and Linux on arm64 or x86_64' ;;
esac

absolute() { python3 -c 'from pathlib import Path; import sys; print(Path(sys.argv[1]).resolve())' "$1"; }
keys="$(absolute "$keys")"
if [[ -n "$disc" ]]; then disc="$(absolute "$disc")"; fi
if [[ -n "$disc_dir" ]]; then disc_dir="$(absolute "$disc_dir")"; fi
if [[ -n "$icon" ]]; then icon="$(absolute "$icon")"; fi
out="$(absolute "$out")"
mkdir -p "$out"/{logs,tools,deps,deko}
lock="$out/.building"
mkdir "$lock" 2>/dev/null || die "another build is using $out (remove $lock if the process was killed)"
trap 'rmdir "$lock"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ -n "$disc" ]]; then
    step 'Extracting your disc'
    disc_hash="$(python3 "$root/switch/standalone/inputs.py" hash "$disc")"
    disc_dir="$out/discs/$disc_hash"
    if [[ ! -f "$disc_dir/.extracted" ]]; then
        nod="$out/tools/nodtool-$nod_asset"
        if [[ ! -x "$nod" ]]; then
            curl -fLSs --retry 3 "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-$nod_asset" -o "$nod.tmp"
            chmod +x "$nod.tmp"
            mv "$nod.tmp" "$nod"
        fi
        [[ ! -e "$disc_dir" ]] || die "incomplete extraction at $disc_dir (choose another --out)"
        mkdir -p "$out/discs"
        extracted="$(mktemp -d "$out/discs/.extract.XXXXXX")"
        if ! "$nod" extract "$disc" "$extracted" >"$out/logs/extract.log" 2>&1; then
            rm -rf "$extracted"
            die "disc extraction failed (see $out/logs/extract.log)"
        fi
        touch "$extracted/.extracted"
        mv "$extracted" "$disc_dir"
    fi
fi

build_id="$(python3 "$root/switch/standalone/inputs.py" validate "$disc_dir" \
    "$upstream" "$root/switch/patches/standalone-runtime.patch" \
    "$root/switch/standalone/translate.sh" "$root/switch/standalone/global.json")"
source_dir="$out/source-$build_id"
if [[ ! -f "$source_dir/.translated" ]]; then
    step 'Fetching the pinned runtime and translating the disc'
    if [[ ! -d "$source_dir/.git" ]]; then
        git init -q "$source_dir"
    fi
    git -C "$source_dir" fetch -q --depth 1 \
        https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation.git "$upstream"
    git -C "$source_dir" checkout -q --detach --force "$upstream"
    git -C "$source_dir" apply --whitespace=nowarn "$root/switch/patches/standalone-runtime.patch"
    cp "$root/switch/standalone/global.json" "$source_dir/global.json"
    bash "$root/switch/standalone/translate.sh" "$source_dir" "$disc_dir" "$root" "$jobs" \
        >"$out/logs/translate.log" 2>&1 ||
        { tail -16 "$out/logs/translate.log" >&2; die 'translation failed'; }
    touch "$source_dir/.translated"
fi

step 'Preparing the Switch toolchain'
image=${NSMBW_SWITCH_IMAGE:-nsmbw-switch-builder:1}
if [[ -z "${NSMBW_SWITCH_IMAGE:-}" ]]; then
    docker build --platform "$platform" -t "$image" -f "$root/switch/standalone/Dockerfile" \
        "$root/switch/standalone"
fi

step 'Compiling the Switch runtime'
pack="$(mktemp -d "$out/.package.XXXXXX")"
trap 'if [[ -n "${staged_nsp:-}" ]]; then rm -f "$staged_nsp"; fi; rm -rf "$pack"; rmdir "$lock"' EXIT
mounts=(
    -v "$root:/recipe:ro" -v "$source_dir:/src"
    -v "$out/deps:/deps" -v "$out/deko:/deko" -v "$pack:/pack"
)
docker_args=(--rm --platform "$platform" --user "$(id -u):$(id -g)" -e HOME=/tmp
    -e NSMBW_BUILD_JOBS="$jobs" "${mounts[@]}" -w /src)
docker run "${docker_args[@]}" "$image" \
    bash /recipe/switch/standalone/build.sh compile 2>&1 | tee "$out/logs/switch-build.log"

step 'Packaging the disc and native executable'
inputs=(-v "$disc_dir:/disc:ro" -v "$keys:/run/nsmbw-keys:ro")
if [[ -n "$icon" ]]; then inputs+=(-v "$icon:/run/nsmbw-icon:ro"); fi
docker run "${docker_args[@]}" --network none "${inputs[@]}" "$image" \
    bash /recipe/switch/standalone/build.sh pack "$version" 2>&1 | tee "$out/logs/pack.log"

result="$out/aaa-nsmbw-v$version-standalone.nsp"
staged_nsp="$(mktemp "$out/.nsp.XXXXXX")"
cp "$pack/out_rb/01004E53A1B50000.nsp" "$staged_nsp"
mv -f "$staged_nsp" "$result"
printf '\nBuilt %s\n' "$result"
