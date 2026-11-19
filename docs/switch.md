# Nintendo Switch

Status: **phase 0 done and confirmed on hardware** (Switch Lite, firmware 20.4.0): the toolchain
builds a `.nro` that runs, and deko3d draws. The translated game's code also compiles for Horizon.
Nothing of the game runs yet.

An NSP needs a Horizon OS build of the game; there is nothing to convert from the macOS/Windows/iOS
binaries. This page records what was proven, what the remaining work is, and how other ports of
comparable games did it.

## What phase 0 proves

`switch/phase0` is the smallest libnx app that proves the toolchain end to end: it links against
libnx, opens the console framebuffer, prints the firmware, the operation mode and the pad state, and
exits on `+`.

```
nsmbw-check.nro   192 KB
```

Run it by copying it to `sdcard:/switch/` and launching it from hbmenu (Atmosphere). It does not
need title installation; it is only a proof.

## Building it

The toolchain lives in a container so nothing is installed system-wide:

```bash
docker build -t nsmbw-switch switch/
docker run --rm -v "$PWD/switch/phase0":/src -w /src nsmbw-switch bash -lc make
```

`switch/Dockerfile` is devkitA64 plus the `switch-dev` group (libnx, deko3d, switch-tools). The image
is ~2.8 GB and only needs building once.

## How other ports did it

- **Shipwright-NX** — a Switch fork of Ship of Harkinian (the Ocarina of Time port). Upstream shipped
  a Switch build on **OpenGL through Mesa** until 8.0.6 and then dropped it; the fork brought it back
  with a **native deko3d renderer** instead. That is the pattern this port would follow: an existing
  recompilation/decompilation, a libnx platform layer, and deko3d for graphics.
  <https://github.com/MintCarasique/Shipwright-NX>
- **Ship of Harkinian's own Switch instructions** distribute a `.nro` on the SD card, launched from
  hbmenu — the same distribution shape as `switch/phase0` above.
- Homebrew on Horizon has **no Vulkan driver and no Metal/D3D12**; the graphics options are
  **deko3d** (native, fast) and **OpenGL via Mesa** (works, slower). That is why the renderer, not
  the build system, is the hard part of this port.

## What this port needs

| Phase | Work | Risk |
|---|---|---|
| 0 ✅ | devkitA64 + libnx toolchain, `.nro` builds and runs | done |
| 1 | Horizon platform layer: window, pad, audren, `sdmc:`/romfs, timing; a sixth platform branch next to Windows/macOS/Linux/Android/iOS | low |
| 2 | **Aurora → deko3d backend**: the GX command stream on deko3d. Start with one textured quad from the game, then a course | **the cliff** |
| 3 | Guest memory: the 4 GiB arena on Tegra, using the iOS launch-time base as the template; saves to `sdmc:` | medium |
| 4 | Packaging: NSP (NCA + NACP) for installation, not a forwarder | medium |

Aurora currently renders through **Dawn** (Metal, D3D12, Vulkan). Horizon has none of those, so phase
2 is a new backend, not a flag. If it cannot be made to work, the project stops there — which is why
it is spiked before the platform layer is built out.

## Resolution

The Switch Lite panel is **1280×720**, so handheld rendering at 720p is native resolution — no
upscale needed, and the port's `resolution_multiplier` maps directly onto it (1.0 = 720p handheld).
Docked output would be 1080p, which is a later concern.

## Testing without a second console

A Switch Lite is enough for bring-up: single player, handheld, 720p. Multiplayer and docked output
need a second unit later. There is no remote-control bridge on Horizon — the useful loop is
`nxlink` over Wi-Fi for deploying and reading the app's stdout.

## Status of this branch

`switch-port` holds Switch work only; the main branch remains the working macOS/Windows/iOS kit.
Nothing here changes those builds.

## What has been proven on the console

| Claim | Evidence |
|---|---|
| devkitA64 + libnx build and run | `nsmbw-check.nro` printed firmware 20.4.0, handheld, pad ok |
| deko3d works on this firmware | the devkitPro deko3d sample drew its triangle (shaders from RomFS) |
| textures, depth, buffers | `deko-examples.nro` (textured cube, teapot, multisampling) built and installed |
| **the translated game compiles for Horizon** | one shard built to a 2 MB object with Clang, `--target=aarch64-none-elf` |

### The translated code needs Clang, not devkitA64's GCC

devkitA64 ships GCC, and the translator's output uses Clang extensions (`__builtin_rotateleft32`,
`__attribute__((ext_vector_type))`). GCC fails on both. Clang can target the Switch sysroot:

```bash
D=/opt/devkitpro/devkitA64/aarch64-none-elf
clang++ --target=aarch64-none-elf -std=gnu++20 -D__SWITCH__ -fPIE \
  --sysroot=$D \
  -isystem $D/include/c++/15.2.0 \
  -isystem $D/include/c++/15.2.0/aarch64-none-elf \
  -isystem $D/include/c++/15.2.0/backward \
  -isystem $D/include \
  -I/opt/devkitpro/libnx/include -I<repo>/runtime/include -I<repo>/runtime/src \
  -c <repo>/generated/build_shards/base_common/shard_*.cpp
```

The include order matters: the C++ headers must come **before** `$D/include`, or `#include_next`
cannot find newlib's `stdlib.h`.

One source change was needed: `runtime/include/system_bridge.h` uses `sigjmp_buf`, which newlib does
not have. On `__SWITCH__` it uses plain `jmp_buf`, which is the whole contract there anyway — the
kernel owns the exception path, so a SIGSEGV handler is not available to homebrew.

`nsmbw-switch-clang` (see `switch/Dockerfile.clang`) adds Clang and LLD to the toolchain image.

## The platform layer cannot be SDL

SDL3 has **no Nintendo Switch backend** — a grep of the SDL3 sources finds zero Switch-specific
files, and devkitPro ships only `libSDL2.a` for Horizon. This port's windowing, events, gamepad and
audio all go through SDL3, and Aurora creates its surface with `SDL_Metal_CreateView`.

So the Switch platform layer replaces SDL rather than configuring it: libnx `nwindow` for the screen,
`HidNpad` for pads, `audren` for audio, `sdmc:`/romfs for storage, and deko3d owning presentation.
That is the same split the Shipwright-NX fork landed on.
