#!/usr/bin/env bash
# Build a standalone Switch NSP from a local SMNE01 revision 1 disc.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

source "$root/switch/standalone/prepare.sh"
prepare_switch_source

step 'Preparing the Switch toolchain'
image=${NSMBW_SWITCH_IMAGE:-nsmbw-switch-builder:1}
if [[ -z "${NSMBW_SWITCH_IMAGE:-}" ]]; then
    docker build --target toolchain --platform "$platform" -t "$image" -f "$root/switch/standalone/Dockerfile" \
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
cp "$pack/out_rb/01004e53a1b50000.nsp" "$staged_nsp"
mv -f "$staged_nsp" "$result"
printf '\nBuilt %s\n' "$result"
