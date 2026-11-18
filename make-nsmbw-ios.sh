#!/bin/bash
#
# make-nsmbw-ios.sh — New Super Mario Bros. Wii as an unsigned IPA for iPhone/iPad.
#
#   ./make-nsmbw-ios.sh --disc "New Super Mario Bros. Wii (USA) (Rev 1).rvz"
#   ./make-nsmbw-ios.sh --disc-dir ~/discs/nsmbw --version 4
#   ./make-nsmbw-ios.sh --repo ~/src/nsmbw --no-translate      (rebuild, sources already translated)
#
#   --disc PATH         disc image (.rvz .iso .wbfs .gcz .wia)   [required unless --disc-dir]
#   --disc-dir PATH     an already extracted disc (sys/ + files/) [optional]
#   --out DIR           work + output folder                     [default: ~/wii-port/nsmbw-ios-build]
#   --repo DIR          an existing checkout of the source       [default: <out>/repo]
#   --version N         version in the IPA name and the app      [default: 1]
#   --jobs N            parallel compile jobs                    [default: all cores]
#   --no-translate      skip the translator (reuse <repo>/generated)
#   --configure-only    stop after the CMake configure (debugging)
#   --clean             wipe the work folder first
#   -h, --help          this text
#
# The translation stage is the same one the desktop scripts use; this script only runs it, then
# configures the same sources for arm64 iOS, builds, and packs an unsigned IPA for LiveContainer.
set -euo pipefail

REPO_URL="https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation.git"
GAME_ID="SMNE01"
APP_NAME="New Super Mario Bros. Wii"
DAWN_VERSION="v20260603.191052"
DAWN_IOS_URL="https://github.com/encounter/dawn-build/releases/download/${DAWN_VERSION}/dawn-ios-arm64.tar.gz"
DAWN_IOS_SHA="a361fcca75929fa5c766cfcde979c010a6da7d805e5db8e15c75e73fd8260e78"
IOS_TARGET="16.0"

DISC=""
DISC_DIR=""
OUT="$HOME/wii-port/nsmbw-ios-build"
REPO=""
VERSION="1"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
NO_TRANSLATE=0
CONFIGURE_ONLY=0
CLEAN=0

die()  { printf '\n\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --disc)          DISC="${2:-}"; shift 2 ;;
        --disc-dir)      DISC_DIR="${2:-}"; shift 2 ;;
        --out)           OUT="${2:-}"; shift 2 ;;
        --repo)          REPO="${2:-}"; shift 2 ;;
        --version)       VERSION="${2:-}"; shift 2 ;;
        --jobs)          JOBS="${2:-}"; shift 2 ;;
        --no-translate)  NO_TRANSLATE=1; shift ;;
        --configure-only) CONFIGURE_ONLY=1; shift ;;
        --clean)         CLEAN=1; shift ;;
        -h|--help)       sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)               die "unknown option: $1 (try --help)" ;;
    esac
done

[[ -n "$DISC" || -n "$DISC_DIR" ]] || die "give --disc <image> or --disc-dir <extracted disc> (--help)"
[[ "$VERSION" =~ ^[0-9]+$ ]] || die "--version takes a number"

REPO="${REPO:-$OUT/repo}"
DISC_DIR="${DISC_DIR:-$OUT/disc}"
BUILD_DIR="$OUT/build-ios"
IPA="$OUT/$APP_NAME v$VERSION.ipa"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HERE="$SCRIPT_DIR"

printf '\n\033[1mNew Super Mario Bros. Wii — iOS build (IPA v%s)\033[0m\n' "$VERSION"

if [[ $CLEAN -eq 1 ]]; then
    step "Cleaning $OUT"
    rm -rf "$OUT"
    ok "removed"
fi
mkdir -p "$OUT/logs"

# ---------------------------------------------------------------- 1. toolchain
step "Checking the toolchain"
[[ "$(uname -s)" == "Darwin" ]] || die "the iOS build needs macOS with Xcode"
command -v xcodebuild >/dev/null 2>&1 || die "xcodebuild not found (install Xcode)"
xcodebuild -showsdks 2>/dev/null | grep -q "iphoneos" \
    || die "no iOS SDK: open Xcode once so it finishes installing, then run xcodebuild -showsdks"
command -v cmake >/dev/null 2>&1 || die "cmake not found (brew install cmake)"
command -v ninja >/dev/null 2>&1 || die "ninja not found (brew install ninja)"
command -v python3 >/dev/null 2>&1 || die "python3 not found"
ok "Xcode $(xcodebuild -version 2>/dev/null | head -1 | awk '{print $2}'), $(xcodebuild -showsdks 2>/dev/null | grep -oE 'iphoneos[0-9.]+' | head -1), cmake $(cmake --version | head -1 | awk '{print $3}')"

# ---------------------------------------------------------------- 2. sources + disc + translation
# The desktop script owns the pipeline: clone, apply the port's fixes, extract, translate. This keeps
# one implementation of that path, and the translated sources are platform independent.
if [[ $NO_TRANSLATE -eq 1 ]]; then
    step "Using the translated sources already in $REPO"
    [[ -f "$REPO/generated/build_shards/shards.cmake" ]] \
        || die "$REPO has no generated/build_shards — run without --no-translate"
    ok "translated sources present"
else
    step "Translating (the desktop script's pipeline)"
    "$HERE/make-nsmbw.sh" --disc "${DISC:-$DISC_DIR/sys/main.dol}" --disc-dir "$DISC_DIR" \
        --out "$OUT" --repo "$REPO" --jobs "$JOBS" --translate-only \
        || die "the translation stage failed (see $OUT/logs)"
    ok "translated"
fi

# ---------------------------------------------------------------- 3. the iOS patch
step "Applying the iOS port"
if grep -q "MKW_PLATFORM_IOS" "$REPO/runtime/CMakeLists.txt" 2>/dev/null; then
    ok "already applied"
else
    git -C "$REPO" apply --verbose "$HERE/patches/ios-port.patch" >"$OUT/logs/ios-patch.log" 2>&1 \
        || { tail -20 "$OUT/logs/ios-patch.log"; die "patches/ios-port.patch did not apply (see $OUT/logs/ios-patch.log)"; }
    ok "applied patches/ios-port.patch"
fi

# ---------------------------------------------------------------- 4. configure
step "Configuring for iOS arm64"
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    cmake -S "$REPO/runtime" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_SYSROOT=iphoneos \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_TARGET" \
        -DAURORA_DAWN_PROVIDER=package \
        -DAURORA_DAWN_LINKAGE=static \
        -DAURORA_DAWN_PACKAGE_URL="$DAWN_IOS_URL" \
        -DAURORA_DAWN_PACKAGE_URL_HASH="SHA256=$DAWN_IOS_SHA" \
        -DMKW_TRANSLATED_SHARD_MANIFEST="$REPO/generated/build_shards/shards.cmake" \
        -DNSMBW_GAME_ID="$GAME_ID" \
        >"$OUT/logs/configure-ios.log" 2>&1 \
        || { tail -25 "$OUT/logs/configure-ios.log"; die "iOS configure failed (see $OUT/logs/configure-ios.log)"; }
    ok "configured (Dawn for iOS arm64, static SDL3)"
else
    ok "already configured"
fi

if [[ $CONFIGURE_ONLY -eq 1 ]]; then
    step "Done (configure only)"
    info "build folder: $BUILD_DIR"
    exit 0
fi

# ---------------------------------------------------------------- 5. build
step "Compiling for iOS (the long one)"
cmake --build "$BUILD_DIR" --target WiiCompiled --parallel "$JOBS" >"$OUT/logs/build-ios.log" 2>&1 \
    || { grep -E "error:" "$OUT/logs/build-ios.log" | head -10; die "iOS compile failed (see $OUT/logs/build-ios.log)"; }
APP_BUNDLE="$BUILD_DIR/WiiCompiled.app"
[[ -f "$APP_BUNDLE/WiiCompiled" ]] || die "the build produced no app bundle"
ok "app: $(du -h "$APP_BUNDLE/WiiCompiled" | awk '{print $1}') arm64 iOS"

# ---------------------------------------------------------------- 6. package
step "Packing $APP_NAME v$VERSION.ipa"
[[ -f "$DISC_DIR/sys/main.dol" ]] || die "no extracted disc at $DISC_DIR (needed for the IPA's game data)"
"$HERE/ios/package-ipa.sh" "$APP_BUNDLE" "$DISC_DIR" "$IPA" "$VERSION" >"$OUT/logs/package-ipa.log" 2>&1 \
    || { tail -15 "$OUT/logs/package-ipa.log"; die "packaging failed (see $OUT/logs/package-ipa.log)"; }
[[ -f "$IPA" ]] || die "no IPA at $IPA"

step "Done"
info "ipa:   $IPA"
info "size:  $(du -h "$IPA" | awk '{print $1}')"
info "logs:  $OUT/logs"
info ""
info "Install with LiveContainer: Import -> Files -> the IPA. Reinstalling keeps the app's data"
info "container only until LiveContainer replaces it, so copy Documents/WiiCompiled/NAND somewhere"
info "safe if the save matters."
