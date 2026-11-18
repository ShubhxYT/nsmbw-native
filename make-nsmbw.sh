#!/bin/bash
# make-nsmbw.sh — build New Super Mario Bros. Wii into a self-contained macOS .app
#
# What it does, in order:
#   1. checks the machine and the toolchain (Xcode CLT, cmake, ninja, dotnet 8, python3)
#   2. fetches nodtool if it is missing (used to extract the disc)
#   3. clones the recompilation source into <out>/repo (or reuses one)
#   4. applies the three local fixes this port needs, idempotently
#   5. extracts your disc and prepares the translator's inputs
#   6. builds the translator, translates the game, compiles the runtime
#   7. packs the result as "New Super Mario Bros. Wii.app" with the game data inside
#
# You need your own copy of New Super Mario Bros. Wii (USA, SMNE01, revision 1).
# Nothing here downloads, contains or redistributes any game code or assets.
#
# Usage:
#   ./make-nsmbw.sh --disc "/path/to/New Super Mario Bros. Wii (USA) (Rev 1).rvz"
#
# Options:
#   --disc PATH       disc image (.rvz .iso .wbfs .gcz .wia)                 [required unless --disc-dir]
#   --disc-dir PATH   an already extracted disc (contains sys/ and files/)  [optional]
#   --out DIR         work + output folder                 [default: ~/wii-port/nsmbw-build]
#   --repo DIR        an existing checkout of the source    [default: <out>/repo]
#   --build-dir DIR   CMake build folder                    [default: <out>/build]
#   --icon PNG        app icon (any size)                   [optional]
#   --jobs N          parallel compile jobs                 [default: all cores]
#   --no-app          stop after the binary, skip the .app
#   --translate-only  stop after the translated sources (the iOS build uses this)
#   --clean           delete <out> first (asks nothing)
#   -h, --help        this text
#
set -euo pipefail

REPO_URL="https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation.git"
APP_NAME="New Super Mario Bros. Wii"
GAME_ID="SMNE01"

DISC=""
DISC_DIR=""
OUT="$HOME/wii-port/nsmbw-build"
REPO=""
BUILD_DIR=""
ICON=""
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
MAKE_APP=1
CLEAN=0
TRANSLATE_ONLY=0

die()  { printf '\n\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '    \033[32mok\033[0m %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --disc)      DISC="${2:-}"; shift 2 ;;
        --disc-dir)  DISC_DIR="${2:-}"; shift 2 ;;
        --out)       OUT="${2:-}"; shift 2 ;;
        --repo)      REPO="${2:-}"; shift 2 ;;
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --icon)      ICON="${2:-}"; shift 2 ;;
        --jobs)      JOBS="${2:-}"; shift 2 ;;
        --no-app)    MAKE_APP=0; shift ;;
        --translate-only) TRANSLATE_ONLY=1; shift ;;
        --clean)     CLEAN=1; shift ;;
        -h|--help)   sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)           die "unknown option: $1 (try --help)" ;;
    esac
done

[[ "$(uname -s)" == "Darwin" ]] || die "this script builds the macOS app; run it on macOS"
[[ "$(uname -m)" == "arm64" ]] || die "this needs an Apple silicon Mac (arm64)"
[[ -n "$DISC" || -n "$DISC_DIR" ]] || die "give --disc <image> or --disc-dir <extracted disc> (--help)"

REPO="${REPO:-$OUT/repo}"
BUILD_DIR="${BUILD_DIR:-$OUT/build}"
DISC_DIR="${DISC_DIR:-$OUT/disc}"
APP="$OUT/$APP_NAME.app"

if [[ $CLEAN -eq 1 && -d "$OUT" ]]; then
    step "Cleaning $OUT"
    rm -rf "$OUT"
fi
mkdir -p "$OUT" "$OUT/logs"

# ---------------------------------------------------------------- 1. toolchain
step "Checking the toolchain"

for tool in git curl python3 cmake ninja dotnet clang++; do
    command -v "$tool" >/dev/null || die "$tool is missing. Install it first (Xcode CLT, Homebrew: cmake ninja, and the .NET 8 SDK)."
done

dotnet --list-runtimes | grep -q 'Microsoft.NETCore.App 8\.' \
    || die "the .NET 8 runtime is missing (dotnet --list-runtimes). Install the .NET 8 SDK."
python3 - <<'PY' || die "this Python is not a native arm64 build (a Homebrew Python from Intel runs under Rosetta)"
import platform, sys
sys.exit(0 if platform.machine() == "arm64" else 1)
PY
xcrun -sdk macosx metal --version >/dev/null 2>&1 \
    || die "the Metal toolchain is missing: xcodebuild -downloadComponent MetalToolchain"
ok "cmake $(cmake --version | head -1 | awk '{print $3}'), ninja $(ninja --version), dotnet $(dotnet --version | cut -d. -f1-2), clang $(clang --version | head -1 | awk '{print $4}')"

# A machine can have .NET installed but no package sources; every restore then fails with NU1100.
if dotnet nuget list source 2>/dev/null | grep -q "No sources found"; then
    dotnet nuget add source https://api.nuget.org/v3/index.json -n nuget.org >/dev/null 2>&1 || true
    ok "added the nuget.org package source (none were configured)"
fi

# ---------------------------------------------------------------- 2. nodtool
step "Disc extraction tool"
NODTOOL="$OUT/tools/nodtool"
mkdir -p "$OUT/tools"
if [[ ! -x "$NODTOOL" ]]; then
    curl -fsSL "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64" -o "$NODTOOL" \
        || die "could not download nodtool"
    chmod +x "$NODTOOL"
    ok "downloaded nodtool"
else
    ok "nodtool already present"
fi

# ---------------------------------------------------------------- 3. source
step "Source code"
if [[ -d "$REPO/.git" ]]; then
    ok "using the checkout at $REPO"
else
    mkdir -p "$(dirname "$REPO")"
    git clone --depth 1 -q "$REPO_URL" "$REPO" || die "could not clone the source"
    ok "cloned into $REPO"
fi
[[ -f "$REPO/projects/nsmbw/recomp.yml" ]] || die "$REPO does not look like the NSMBW recompilation (projects/nsmbw/recomp.yml missing)"

# ---------------------------------------------------------------- 4. fixes
step "Applying this port's fixes (idempotent)"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
python3 "$SCRIPT_DIR/apply-patches.py" "$REPO" || die "could not apply the port's fixes"

# ---------------------------------------------------------------- 5. disc
step "Your disc"
if [[ -f "$DISC_DIR/sys/main.dol" ]]; then
    ok "already extracted: $DISC_DIR"
else
    [[ -n "$DISC" ]] || die "no extracted disc at $DISC_DIR and no --disc given"
    [[ -f "$DISC" ]] || die "disc image not found: $DISC"
    mkdir -p "$DISC_DIR"
    info "extracting $(basename "$DISC") …"
    "$NODTOOL" extract "$DISC" "$DISC_DIR" >"$OUT/logs/extract.log" 2>&1 || die "nodtool could not read the disc (see $OUT/logs/extract.log)"
    [[ -f "$DISC_DIR/sys/main.dol" ]] || die "the disc did not contain sys/main.dol"
    ok "extracted ($(du -sh "$DISC_DIR" | awk '{print $1}'))"
fi

step "Preparing the translator's inputs"
cd "$REPO"
# A dump whose main.dol differs from the table is accepted with a warning: disc identity, FST size,
# entry point and the REL prelink all still validate it, so the mismatch usually means a patched DOL.
python3 "$SCRIPT_DIR/apply-patches.py" "$REPO" --disc "$DISC_DIR" || die "could not adapt the checksum table"

python3 projects/nsmbw/tools/unpack_game.py "$DISC_DIR" >"$OUT/logs/unpack.log" 2>&1 \
    || { tail -5 "$OUT/logs/unpack.log"; die "unpack_game.py failed"; }
ok "game code unpacked"
python3 projects/nsmbw/tools/prelink_rels.py >"$OUT/logs/prelink.log" 2>&1 \
    || { tail -10 "$OUT/logs/prelink.log"; die "prelink_rels.py failed"; }
ok "$(grep -c 'predicted .* \[ok\]' "$OUT/logs/prelink.log" 2>/dev/null || echo 0)/3 REL bases predicted correctly"

# ---------------------------------------------------------------- 6. translate + build
step "Translating (39k functions)"
mkdir -p generated/functions build/base generated/build_shards
dotnet build translator/Translator.sln -c Release -v quiet -nologo >"$OUT/logs/translator-build.log" 2>&1 \
    || die "the translator did not build (see $OUT/logs/translator-build.log)"
TR="translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
run_tr() { dotnet "$TR" "$@" >>"$OUT/logs/translate.log" 2>&1 || { tail -15 "$OUT/logs/translate.log"; die "translator step failed: $1"; }; }

run_tr translate-recursive 0x80004050 --project projects/nsmbw/recomp.yml --outdir generated/functions \
    --output-metadata generated/base_translation_output.json --production-source-bundle generated/base_translation_sources.bin \
    --no-function-files --prune-stale --threads "$JOBS"
ok "code translated"
run_tr emit-base-manifest --project projects/nsmbw/recomp.yml --out build/base --functions-dir generated/functions \
    --translation-output-metadata generated/base_translation_output.json --region E
run_tr generate-data-init --project projects/nsmbw/recomp.yml
run_tr emit-build-shards --project projects/nsmbw/recomp.yml --base-metadata generated/base_translation_output.json \
    --base-functions-dir generated/functions --native-source-dir runtime/src --out generated/build_shards
ok "build shards emitted"

if [[ $TRANSLATE_ONLY -eq 1 ]]; then
    step "Done"
    info "translated sources: $REPO/generated/build_shards"
    info "next: build them for the target platform (make-nsmbw.sh for an app, make-nsmbw-ios.sh for an IPA)"
    exit 0
fi

step "Compiling the runtime (this is the long one)"
mkdir -p "$BUILD_DIR"
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    cmake -S runtime -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DAURORA_SDL3_PROVIDER=vendor -DNSMBW_GAME_ID="$GAME_ID" \
        >"$OUT/logs/configure.log" 2>&1 || { tail -20 "$OUT/logs/configure.log"; die "CMake configure failed"; }
    ok "configured"
fi
cmake --build "$BUILD_DIR" --target WiiCompiled --parallel "$JOBS" >"$OUT/logs/build.log" 2>&1 \
    || { grep -E "error:" "$OUT/logs/build.log" | head -10; die "compile failed (see $OUT/logs/build.log)"; }
[[ -x "$BUILD_DIR/WiiCompiled" ]] || die "the build produced no binary"
ok "binary: $(du -h "$BUILD_DIR/WiiCompiled" | awk '{print $1}') arm64"

# ---------------------------------------------------------------- 7. the app
if [[ $MAKE_APP -eq 1 ]]; then
    step "Packing $APP_NAME.app"
    rm -rf "$APP"
    mkdir -p "$APP/Contents/MacOS/bin" "$APP/Contents/Resources"

    cp "$BUILD_DIR/WiiCompiled" "$APP/Contents/MacOS/bin/WiiCompiled"
    chmod +x "$APP/Contents/MacOS/bin/WiiCompiled"
    for asset in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap; do
        [[ -e "$BUILD_DIR/$asset" ]] && cp -R "$BUILD_DIR/$asset" "$APP/Contents/MacOS/bin/"
    done
    cp -R "$DISC_DIR" "$APP/Contents/Resources/game"

    cat > "$APP/Contents/MacOS/NSMBW" <<'LAUNCH'
#!/bin/zsh
# Start the game. The bundled copy of the disc is the default game folder; a Config.toml that
# already names one is left alone, so a player who moved their files keeps their path.
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="$HERE/../Resources/game"
CFG="$HOME/Library/Application Support/WiiCompiled/Config.toml"
mkdir -p "$(dirname "$CFG")"
if [[ ! -f "$CFG" ]]; then
    printf '[video]\ngraphics_api = "metal"\n\n[paths]\ndvd_root = "%s"\n' "$GAME" > "$CFG"
elif ! grep -q '^[[:space:]]*dvd_root' "$CFG"; then
    awk -v game="$GAME" '
        BEGIN { done = 0 }
        /^\[paths\]/ { print; print "dvd_root = \"" game "\""; done = 1; next }
        { print }
        END { if (!done) { print "[paths]"; print "dvd_root = \"" game "\"" } }
    ' "$CFG" > "$CFG.tmp" && mv "$CFG.tmp" "$CFG"
fi
cd "$HERE/bin" || exit 1
exec ./WiiCompiled
LAUNCH
    chmod +x "$APP/Contents/MacOS/NSMBW"

    if [[ -n "$ICON" ]]; then
        [[ -f "$ICON" ]] || die "icon not found: $ICON"
        ICONSET="$OUT/AppIcon.iconset"
        rm -rf "$ICONSET"; mkdir -p "$ICONSET"
        for size in 16 32 128 256 512; do
            sips -z $size $size "$ICON" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null 2>&1
            sips -z $((size*2)) $((size*2)) "$ICON" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null 2>&1
        done
        iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" >/dev/null 2>&1 \
            && rm -rf "$ICONSET" && ok "icon embedded"
        ICON_KEY='  <key>CFBundleIconFile</key><string>AppIcon</string>'
    else
        ICON_KEY=''
    fi

    cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleExecutable</key><string>NSMBW</string>
  <key>CFBundleIdentifier</key><string>local.nsmbw.recomp</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
$ICON_KEY
</dict>
</plist>
PLIST

    codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || info "ad-hoc signing skipped"
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP" >/dev/null 2>&1 || true

    # Desktop shortcut: a Finder alias (a plain link if osascript is unavailable). Reruns keep one.
    if [[ ! -e "$HOME/Desktop/$APP_NAME.app" ]]; then
        osascript -e "tell application \"Finder\" to make alias file to (POSIX file \"$APP\") at (path to desktop folder)" >/dev/null 2>&1 \
            || ln -sfn "$APP" "$HOME/Desktop/$APP_NAME.app"
    fi
    ok "desktop shortcut: ~/Desktop/$APP_NAME.app"

    step "Done"
    info "app:  $APP"
    info "size: $(du -sh "$APP" | awk '{print $1}')  (game data included)"
    info "run:  open \"$APP\""
    info "saves: ~/Library/Application Support/WiiCompiled/NAND"
    info "logs:  ~/wii-port/nsmb-run.log and ~/Library/Application Support/WiiCompiled/Logs"
    info "F10 in game: resolution, aspect (4:3 / 16:10 / 16:9 / fill), display mode, controls, button labels"
else
    step "Done"
    info "binary: $BUILD_DIR/WiiCompiled"
fi
