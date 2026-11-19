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

## The development loop

Working, on a Switch Lite (20.4.0):

```
build (container)  ->  push (switch/nxlink.py)  ->  app runs  ->  its log comes back
```

One button press per build, which is the norm for homebrew: hbmenu's netloader accepts a single
connection per activation (**Y** in hbmenu), and there is no maintained sysmodule that removes that
step (`sys-nxlink` is a 2018 proof of concept that only survives one session).

`switch/nxlink.py` is a small client for that protocol because the container cannot reach the LAN on
macOS. It sends the `.nro`, then listens on 28281 for the app's own log stream. Two details that cost
time to find:

- hbmenu's inflate buffer is `ZLIB_CHUNK` = **16 KB**, so each compressed chunk on the wire must be
  smaller than that. Larger chunks are refused with "Invalid chunk size".
- The netloader is **one connection per activation**. Probing the port with `nc` consumes it.

Apps log back by dialling the host themselves (`Say()` in `switch/phase0/source/main.c`): hbmenu does
not pass the host to the app here, so `nxlinkStdio()` has nothing to connect to.

## What the runtime needs from Horizon

Measured from the runtime sources, before any of it is ported:

| Dependency | Files / sites | Consequence |
|---|---|---|
| SDL3 | 17 files | replaced wholesale: `nwindow` for the screen, `HidNpad` for pads, `audren` for audio |
| `std::filesystem` | 47 files | **the largest port surface**; newlib's libstdc++ support is thin |
| threads | 36 sites | mapping onto Horizon threads |
| `mmap` / `mprotect` | 12 sites | the guest arena and its page protection |
| `<unistd.h>` | 16 files | mostly `usleep`, `getpid`, file calls — newlib covers most |
| sockets | 3 files | only network play; can be stubbed |

Note what is *not* on that list: the graphics backend is a separate problem, and still the cliff.

## Memory on Horizon, measured

A probe built with the same container (a Switch Lite, firmware 20.4.0, launched as an application)
reported:

```
process memory: total 3189 MB, used 3185 MB
alias region:   6144 MB at 0x90e00000
heap region:    8192 MB at 0x7e1200000
alloc+write 512 MB: ok
```

So the guest arena is not the obstacle it looked like:

- ~3.1 GB of memory is available to the process; the runtime needs 88 MB of guest RAM and held about
  1.1 GB resident on Windows.
- A 6 GB alias region and an 8 GB heap region give the address space a 4 GiB arena needs.
- A 512 MB allocation commits and writes without complaint.

The shape of the work is therefore a mapping change, not a redesign: Horizon has **no `mmap` and no
general reservation API** (the probe would not even compile with `<sys/mman.h>`), so the flat arena
has to be placed in the alias region with the kernel's memory mapping calls instead of the portable
mmap path the other platforms use.

## Compiling the runtime for Horizon, measured

With the toolchain fixed (Clang 19 against devkitA64's libstdc++ 15 — Clang 14 cannot parse it), the
runtime's translation units were compiled for `aarch64-none-elf` one by one:

| File | Result |
|---|---|
| `det_clock.cpp` | **compiles clean** |
| `memory.cpp` | **compiles clean** |
| `guest_flat_memory.cpp` | one wall: `sys/mman.h` does not exist on Horizon |
| `game_folder.cpp`, `wii_remote_input.cpp` | include-path gap only (`toml.hpp`) |
| `main.cpp` | needs SDL3, which has no Switch backend |

So the port's shape is now concrete rather than guessed:

1. **Guest memory**: replace the mmap path with the alias-region mapping (the probe showed a 6 GB
   alias region and a 512 MB allocation that commits).
2. **Platform layer**: replace SDL for window, pad, audio and timing with libnx.
3. **Everything else** — the timing, memory and game logic — is already compiling.

## Threads and std::filesystem work — measured

Probe on the console:

```
threads:       4 x 100000 increments -> 400000 (29 ms)
chrono sleep:  50 ms requested, 50 ms measured
fs mkdir:      ok
fs write:      ok
fs size:       28 bytes
fs iterate:    50 entries in sdmc:/switch
```

That removes two entries from the earlier "biggest port surface" list: devkitA64's libstdc++ provides
**working `std::thread`** (pthread-backed) and a **working `std::filesystem`** on `sdmc:`. The 36
thread sites and 47 filesystem files do not need replacing.

Which leaves exactly two walls for the whole port:

1. **SDL3** — no Switch backend; replaced by a libnx platform layer (window, pad, audio).
2. **Guest memory** — Horizon has no `mmap`; the flat arena must be mapped into the alias region.

## The guest arena, implemented

`switch/patches/guest-memory.patch` is the Horizon memory model, and `guest_flat_memory.cpp` now
compiles clean for `aarch64-none-elf`:

- **No `mmap`**: Horizon has none, so the arena is one contiguous 512 MB heap block covering the range
  the game addresses (MEM1 at `0x80000000`, MEM2 at `0x90000000`), presented as the flat base.
- **No fault handler**: the kernel owns the exception path, so `g_requiresCheckedAccess` is forced on
  and guest access is bounds-checked instead of protected by page permissions. The runtime already
  had this mode for hosts with unusual page sizes.
- **MMIO needs no storage**: the checked accessors divert the `0xCC000000` register window, so the
  arena does not have to span it.
- `sigjmp_buf` again: newlib has no such type, so `system_bridge.h` uses plain `jmp_buf`.

The POSIX mapping calls the file still contains are compiled out on Switch through small shims; they
are unreachable in checked mode.

That leaves the runtime with **one** wall: SDL.
