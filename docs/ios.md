# Running it on an iPhone or iPad

The port builds for iOS from the same sources as the Mac app. The result is an **unsigned IPA** for
LiveContainer, SideStore or any other sideloading tool — it is not an App Store app, and there is no
emulator anywhere in it: the game's own code runs on the Apple chip, with Switch-style work standing
in for the Wii's hardware.

Status: **working on iPad** — game runs, controllers work (buttons, sticks, rumble), saves persist.
Two known gaps, both listed at the end.

## What you need

- **Xcode** with an iOS SDK (`xcodebuild -showsdks` should list `iphoneos`).
- **The patch in this repository**: `patches/ios-port.patch`, applied on top of the upstream sources
  the other platforms use.
- **Dawn for iOS arm64**, prebuilt: `encounter/dawn-build` at the version Aurora pins. Its SHA-256 is
  in `runtime/CMakeLists.txt` of the patched tree; the build fails closed on a mismatch.
- Your own disc, extracted exactly as for the Mac app (`sys/` + `files/`).

## Build

```bash
# 1. Get the upstream sources the other platforms use, then apply this repository's fixes
python3 apply-patches.py <checkout>
git -C <checkout> apply /path/to/nsmbw-native/patches/ios-port.patch

# 2. Configure for iOS. The Dawn archive is passed explicitly: Aurora's default provider only
#    publishes desktop builds at this version.
cmake -S <checkout>/runtime -B <checkout>/build-ios -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DAURORA_DAWN_PROVIDER=package \
  -DAURORA_DAWN_LINKAGE=static \
  -DAURORA_DAWN_PACKAGE_URL="https://github.com/encounter/dawn-build/releases/download/v20260603.191052/dawn-ios-arm64.tar.gz" \
  -DAURORA_DAWN_PACKAGE_URL_HASH="SHA256=a361fcca75929fa5c766cfcde979c010a6da7d805e5db8e15c75e73fd8260e78" \
  -DMKW_TRANSLATED_SHARD_MANIFEST=<checkout>/generated/build_shards/shards.cmake \
  -DNSMBW_GAME_ID=SMNE01

# 3. Build the app bundle (this compiles ~40,000 translated functions for arm64 iOS)
cmake --build <checkout>/build-ios --target WiiCompiled --parallel "$(sysctl -n hw.ncpu)"

# 4. Package
./ios/package-ipa.sh <checkout>/build-ios/WiiCompiled.app <game folder> ~/Downloads/NSMBW.ipa 1
```

The build is incremental: the translated sources, the runtime and the dependencies stay cached, so a
later change rebuilds only what moved.

## What the iOS patch changes, and why

Everything below is a real gap that stopped the app launching, rendering or saving. They are grouped
so the next person can find the one that matters to them.

### It had no iOS platform at all

| File | Change |
|---|---|
| `runtime/CMakeLists.txt` | `MKW_PLATFORM_IOS` branch of its own (any Apple target was treated as macOS), Dawn's Metal backend, the AArch64 context switch, and **static dependencies** — a shared libpng linked at an `@rpath` is a missing library on a device |
| `aurora-main/lib/webgpu/metalfx.mm` | iOS ships `IOSurfaceRef.h`, not the macOS `IOSurface.h` umbrella header |
| `runtime/src/external_audio_macos.cpp` | iOS has no Core Audio *process object* graph; the query is stubbed to "nothing playing" |
| `runtime/src/guest_flat_memory.cpp` | iOS has no `memfd_create`; guest RAM is an unlinked file in the app's own `TMPDIR` |

### Guest memory cannot live at a fixed address

The translated code reaches guest memory through `MemoryInline::FlatRead32(offset)` — it never embeds
a base address. The base is a single macro. On desktop that macro is a compile-time constant (a
512 GiB region reserved at startup); iOS gives an app a smaller address space and promises no
particular address, so the patch:

- reserves the 4 GiB guest arena **wherever the kernel puts it**,
- publishes that address in `GuestFlat::gFlatGuestBase`,
- points `MKW_FLAT_GUEST_BASE` at it on iOS.

Cost: one extra pointer load per guest access on iOS, none on the other platforms. No re-translation.

### There is no launcher to start the app

| File | Change |
|---|---|
| `runtime/src/main.cpp` | `SDL_MAIN_HANDLED` is set for the product, so nothing sets up `UIApplicationMain`. On iOS `main` now calls `SDL_RunApp`, which runs the app lifecycle and calls back into the runtime |
| `runtime/src/game_folder.cpp` | No launcher writes `dvd_root`, and there is no file picker: the app uses the `game/` folder inside its own bundle |
| `runtime/src/platform/host_platform.cpp` | Data (config, saves, logs) goes to `Documents/WiiCompiled` instead of `Library/Application Support`, so the Files app can reach it |

### Controller defaults

| File | Change |
|---|---|
| `runtime/include/runtime_config.h` | New installs start with Xbox button names, `z = "right_trigger@50"`, `tilt_left/right` on the shoulders, and 4× resolution |
| `runtime/src/wii_remote_input.cpp` | Sensors are enabled on every sample (the previous id-comparison could never pass when the first controller reported id 0), a one-time line reports whether SDL sees an accelerometer, and **L1/R1 roll the remote** as a tilt fallback |

### SDL's iOS backend

`runtime/CMakeLists.txt` also patches SDL's `SDL_mfijoystick.m` after it is fetched: DualShock 4 and
DualSense require manual motion activation on iOS (`sensorsRequireManualActivation`), which SDL never
does, so motion stays dormant. This is applied at configure time and is idempotent.

## Installing

The IPA is unsigned. In **LiveContainer**: *Import* → *Files* → the folder holding the IPA. It
replaces a previous copy with the same bundle ID; the app's data container is separate.

Every build carries its version in `build-fingerprint.json`, and the runtime prints it as the first
line of the log (`[runtime] WiiCompiled 3`), so a log always says which build produced it.

## On the device

| What | Where (Files app, under the app's name) |
|---|---|
| Logs | `WiiCompiled/Logs/<run>/console.log` |
| Config | `WiiCompiled/Config.toml` |
| Save | `WiiCompiled/NAND/title/00010000/534d4e45/data/wiimj2d.sav` |

Under LiveContainer those live inside its own container:
`Data/Application/<uuid>/Documents/WiiCompiled/…`.

**Keep backups outside that container.** LiveContainer replaces the data container when an app is
reinstalled, and it deletes the old one; anything kept only inside it goes with it. A folder of your
own outside `Data/` is safe.

## Known gaps

### Controller gyro does not reach the app

The DualShock 4's accelerometer is not delivered to third-party apps on current iPadOS. This is not
specific to this port: Moonlight's iOS app has the same defect open (its issue #725, iPadOS 27:
buttons, sticks and rumble work; `GCMotion` data never arrives, even though the source handles it),
and its community workaround is to use the *device's* gyro instead.

Practical consequence: tilt the remote with **L1 / R1** (built in), which covers the tilting
platforms and the other motion spots that need a roll.

### A world-map crash

See [known-issues.md](known-issues.md): leaving a course for the world map can stop the game. It is
deterministic, unrelated to networking, and open.
