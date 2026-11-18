#!/bin/bash
# Assemble an unsigned IPA for LiveContainer from a built iOS app and the game's data.
#
#   ./ios/package-ipa.sh <WiiCompiled.app> <extracted game folder> <output.ipa> [version]
#
# <WiiCompiled.app> is what `cmake --build build-ios --target WiiCompiled` produces.
# <extracted game folder> is the disc, extracted the same way as for the Mac app (sys/ + files/).
# [version] goes into the bundle and into build-fingerprint.json, so the runtime's log says it.
set -euo pipefail

APP_IN="${1:?usage: package-ipa.sh <WiiCompiled.app> <game folder> <output.ipa> [version]}"
GAME="${2:?usage: package-ipa.sh <WiiCompiled.app> <game folder> <output.ipa> [version]}"
OUT="${3:?usage: package-ipa.sh <WiiCompiled.app> <game folder> <output.ipa> [version]}"
VERSION="${4:-1}"
HERE="$(cd "$(dirname "$0")" && pwd)"

[[ -d "$APP_IN" ]] || { echo "no app at $APP_IN" >&2; exit 1; }
[[ -d "$GAME/sys" ]] || { echo "no extracted game at $GAME (expected $GAME/sys)" >&2; exit 1; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

APP="$STAGE/Payload/NSMBW.app"
mkdir -p "$STAGE/Payload"
cp -R "$APP_IN" "$APP"

# The plist carries the icon set, the iPad-only orientation and the gamepad declarations.
cp "$HERE/Info.plist" "$APP/Info.plist"
cp "$HERE"/icon-set/*.png "$APP/"

# The runtime reads this for the version it prints at the top of every log.
printf '{\n  "SetupVersion": "%s",\n  "Platform": "ios"\n}\n' "$VERSION" > "$APP/build-fingerprint.json"

# The game's own data, unchanged: the port reads it exactly as it does on the Mac.
cp -R "$GAME" "$APP/game"

/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION.0" "$APP/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $VERSION" "$APP/Info.plist" 2>/dev/null || true

# Ad-hoc signature: LiveContainer re-signs on install, but the payload has to be coherent.
codesign --force --sign - --timestamp=none "$APP" >/dev/null 2>&1 || true

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
(cd "$STAGE" && zip -qry "$OUT" Payload)

echo "ipa:  $OUT"
echo "size: $(du -h "$OUT" | awk '{print $1}')"
