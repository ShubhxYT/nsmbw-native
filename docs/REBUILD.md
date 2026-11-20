# NSMBW on Nintendo Switch — full project state and rebuild guide

**Purpose of this document.** Everything needed to understand, recreate, or continue this project from
nothing, without the working tree. It records the goal, what has been verified, platform fixes, the
build environment, and the remaining work.

**Current status.** v1.0.20 runs at 1× on the stock-clock Switch Lite with working controls.
Its latest course capture averaged 52.1 FPS in the final 30 seconds and 46.2 FPS overall.
Busy scenes exceed the 16.7 ms frame budget. See the
[v1.0.20 hardware measurements](#v1020-staging-rollback-and-presentation-timing).

---

## 0. Never commit — hard rule

Two things must never enter this repository, in any branch, commit or tag:

* **The extracted game data.** The disc's `sys/` and `files/` trees, `main.dol`, the FST, and any
  archive extracted from them.
* **Any console keys.** `prod.keys`, `title.keys`, the trimmed keyset used for packaging, tickets, or
  anything derived from them.

This document refers to both by **path and description only**, deliberately, so the guide stays usable
on a clean machine where the person rebuilding supplies their own legally obtained copy. If either has
ever been staged, deleting it in a later commit is not enough — it has to be purged from history,
because a repository is the wrong place for it and history keeps it forever.

---

## 1. Goal and definition of done

Run *New Super Mario Bros. Wii* natively on a jailbroken Switch Lite, not via an emulator inside the
console. Delivered as an installable **NSP** (an `.nro` was explicitly rejected). Target 720p,
single-player, no LAN or online play.

"Done" means **fully playable**: installs from the home menu, boots, reaches the game's own menus,
accepts input, plays audio, and levels can actually be played through. A single presented frame is a
milestone on the way, **not** the acceptance bar.

---

## 2. Window and frame presentation

The original surface creation failure left Ryujinx on its loading splash.

Evidence chain, each line produced by an actual run:

| Measurement | Value |
|---|---|
| `nwindowIsValid(nwindowGetDefault())` | **0 (invalid)** |
| `eglCreateWindowSurface(...)` return | `EGL_NO_SURFACE` |
| `eglGetError()` | **`0x300b` = `EGL_BAD_NATIVE_WINDOW`** |
| Dawn's message | `Couldn't create an EGLSurface for [Surface "Surface"]` |
| Consequence | WebGPU device lost → Aurora fatal renderer error → no frame |

So Dawn and Aurora are correct; the NWindow handed to EGL is simply not a valid window object. Mesa's
Switch EGL checks `nwindowIsValid` before anything else and refuses it.

**Why the default window is invalid:** libnx creates the default NWindow during its own application
startup. This port **overrides `__appInit`**, so that startup never runs. Same class of problem as the
NPDM service table (§4.3).

The port now creates its window explicitly:

1. In `__appInit`, after `appletInitialize()`: initialise VI as the application.
2. Open the default display and create a VI layer on it.
3. Create an NWindow from that layer, store it once in a global.
4. Have **both** the SDL platform layer and Aurora's surface descriptor use that handle, falling back
   to the default only if creation failed.

The explicit window verified as `valid=1`, and `eglCreateWindowSurface` succeeded. Two additional
changes let the renderer present the startup overlay:

- The [Dawn queue patch](../switch/patches/dawn-switch-fence-fallback.patch) calls `glFinish` when Mesa
  exposes no EGL fence sync API. This avoids a call through a null function pointer on first submit.
- The [Dawn presentation patch](../switch/patches/dawn-switch-present.patch) uses the native window
  dimensions when Mesa returns a 0×0 EGL surface. The native window reports 1280×720, and Ryujinx
  displayed the port's credits overlay once Dawn used those dimensions for the final blit.

### The guest picture

The startup overlay rendered but the guest picture stayed black. A synthetic GX quad rendered
through the same viewport and display copy. OpenGL readback showed the expected vertex bytes in the
upload and destination buffers, but the vertex shader read different values through its storage
binding. Apply the [Aurora vertex texture patch](../switch/patches/aurora-switch-vertex-textures.patch)
to route vertex and indexed attribute reads through `R32Uint` textures on Switch. The patch keeps
the packed GX data and shader decoders intact.

The first confirmed guest picture was the Wii Remote wrist strap screen. Ryujinx ran at about
60 FPS and the VI watchdog counted 1,730 guest frames at 30 seconds. The watchdog's
`first visible guest frame presented` message indicates a display copy with VI black disabled.
Verify the actual picture in the viewport.

### Wii system memory

`NANDSafeOpen` stopped at `wiimj2d.sav` because the standard library's file copy returned
`Function not implemented` on the Switch SD mount. The shared shadow-copy code now reads and writes
bytes directly on Horizon. Ryujinx also rejects a rename over an existing destination, so the save
commit flushes the scratch data, renames the original to a sibling backup, and publishes the scratch
file under the original name. The next NAND access recovers the original if publishing was
interrupted before the new file took its place.

Ryujinx created a 16,288-byte `wiimj2d.sav` and a 29,344-byte `banner.bin` from a fresh game-save
directory. The guest displayed its save-created confirmation. To simulate an interruption before
publish, the new save was moved to `.nandsafe.bak` with the emulator stopped. On restart the game
restored the original filename, read all 16,288 bytes, and reached the title screen. The restored
save matched the original byte for byte.

### Controller input

The Switch SDL shim samples the libnx pad and publishes both SDL events and polled button and axis
state. On connection or disconnection it sends the device event Aurora uses to assign Player 1.
The virtual pad has a stable SDL handle. Ryujinx's keyboard-to-Pro-Controller A mapping opened
the file selection menu.

---

## 3. What is proven working

Every item below was confirmed by a run on hardware or in Ryujinx, not assumed.

- NSP packaging, install as a base application, and launch from the home menu.
- The dynamic loader path: 122 C++ static initialisers all run and return.
- TLS: thread-local storage works, including for threads created at runtime.
- libc/libstdc++: `printf` conversions, `setlocale`, `newlocale`, `std::locale::classic()`, `<iostream>`.
- Abseil, fmt, mbedTLS, SQLite, zlib, libpng, cryptopp link and run.
- Guest memory: a 512 MiB arena, page tables, fast-path tables, low-memory seeding.
- The extracted disc's embedded DOL/REL data sections are loaded.
- The guest reaches the DOL constructor loop and proceeds into its boot scene.
- The translated-function registry holds the full map (not the truncated 396 entry subset).
- The guest executes translated PowerPC code and dispatches indirectly.
- **Aurora + Dawn + Mesa EGL initialise successfully**: an OpenGL ES 3.2 adapter is found and the
  WebGPU device is created (`aurora status=0`), with the renderer reaching swapchain setup.
- Zero unhandled C++ exceptions in the current build.

The Switch Lite has displayed gameplay and accepted controller input. The hardware logs show
Wii save creation and nonzero audio output samples. Saving during a full level run, reloading that
save, and audible playback on hardware still need verification.

---

## 4. Every platform defect found, in the order discovered

This is the most valuable part of the document. Each entry is symptom → cause → fix. All of them are
peculiar to running a large C++ program as an NSP on Horizon; none is a bug in the game.

### 4.1 The `.nro` was the wrong delivery format
hbmenu's loader crashes on a 103 MB image. Ship an NSP instead.

### 4.2 Title ID shape decides how a title is classified
The console refuses to install anything it reads as an update or addon ("Game uses titleID from
update", "Creating game… Failed 0x2e4"). A **base application** title ID must begin `0100`, have
non-zero middle groups, and end `0000`. The working value is `01004E53A1B50000`.

### 4.3 The native application needs an explicit service list

The earlier NSP shipped an NPDM without the applet, video, GPU, and input services used during
startup. The Switch log recorded applet error `0x5759` and video error `0x1015` before window
creation. The package now builds its NPDM from `switch/game.npdm.json`, including `apm`, `appletOE`,
`vi:u`, `nvdrv`, `nvdrv:a`, and `hid`. The generated binary includes the entries in both access
tables. The updated package launches on the Switch Lite and reaches gameplay.

The custom `__appInit` logs the applet and window setup results. It uses raw file writes because
the CRT has yet to initialize stdio.

### Hardware stall after window creation

The v1.0.1 hardware trace reached `nwindowCreateFromLayer` successfully and stopped inside the
diagnostic that replaced the C++ constructor table. The v1.0.2 build removes that diagnostic and
lets the normal startup code call the constructors. The `CRT: logger opened` and `MAIN` markers
identify the boundaries of that stage.

### 4.4 `.dynamic` had no `DT_NULL` terminator
libnx links with no spare dynamic tags, so `__nx_dynamic` walked past the section into garbage and
faulted. **Fix: patch the last `.dynamic` entry to `DT_NULL` after linking.** Reported symptom is a
crash inside `_dynProcessRela`.

### 4.5 Read-only relocations into `.rodata`
devkitPro's prebuilt libraries are non-PIC and put vtables and locale tables in `.rodata`. The NSO
loader maps that segment read-only, so every relative relocation there fails, and Atmosphère reports
it as "Fault Address: 0".

**The eventual fix is much better than patching libraries:** move `.rodata` into the writable segment
in the linker script. Deviations that matter:

- The `.rodata` block must be **moved to the end** of the read-only group, not retargeted in place;
  sections that follow it (`.tls.align`, `.eh_frame`, `.gnu.version*`, build-id) stay read-only, and a
  read-only section placed after a writable one produces overlapping LMAs.
- The custom script must be fed via a **modified specs file**, because passing a second `-T` on the
  command line leaves both scripts active and resets the location counter.
- The read-only segment must remain non-empty or the NSO converter's "exactly three loadable
  segments" requirement fails.

### 4.6 Blanket re-PIC of libc was wrong, and is no longer needed
An earlier approach renamed every `.rodata` section in six devkitPro archives. That silently broke
newlib's locale tables: literals still printed, but **every conversion vanished** (`%d`, `%s`, `%c` all
empty), `setlocale`/`newlocale` returned NULL, and `<iostream>`'s initialiser died. This is why game
logs showed `(log )` with a blank.

With §4.5 in place the libraries link **unmodified**, so the surgical version is all that could ever be
justified: rename only the section named by a `.rela.<section>` entry. On these libraries that is 821
sections in libstdc++, 24 in libstdc++fs, **9 in libc**, 1 in libsysbase, none in libm/libgcc.

### 4.7 TLS was never bootstrapped — the deepest bug in the project
An NSO carries no `PT_TLS`, and **nothing in libnx ever writes `tpidr_el0`** (libnx reads
`tpidrro_el0`, which is the kernel's IPC buffer). So `tpidr_el0` stays 0 and every `thread_local`
dereferences `0 + offset`. Atmosphère reported all of it as "Fault Address: 0".

Three further subtleties, each of which cost a cycle:

- The linker computes TLS offsets against a base that is **not** the start of `.tdata`, so the base has
  to be calibrated at runtime against a sentinel `thread_local` rather than assumed.
- The sentinel read must be in a **non-inlined** function, or the compiler hoists the register read out
  of the calibration loop and faults before calibration begins.
- The block must be the reserved **`.main.tls`** region, not `.tdata`. The linker gives `.tbss` and
  `.bss` the *same* virtual address, so pointing at `.tdata` makes every `.tbss` thread-local alias a
  real global — it clobbered the translated-function registry's storage pointer, which is why static
  initialisation stopped mid-way for several builds.
- Threads created later inherit nothing. `pthread_create` must be wrapped so each new thread attaches
  its own TLS block.

### 4.8 Abseil registration aborted during static init
Acquiring a flag's mutex during static initialisation causes an instruction fault on Horizon, killing
the process before `main`. The runtime never uses absl flags, so registration and the flag callback are
made no-ops.

### 4.9 The guest arena could not be a fixed address
Every other platform reserves the flat guest space at a fixed base, and the generated code adds a
**compile-time constant** to guest addresses. Horizon cannot choose an allocation address, so the base
must become a **runtime value** and every unit that inlines it must be rebuilt — runtime, all 72
translated shards, and all 16 registration shards. Symptom was an invalid-access exception at
`0x40816FFFF0` (constant base + a MEM1 address).

### 4.10 The guest bootstrap took an unimplemented shared-memory path
The arena setup unconditionally entered a `memfd_create`/`mmap` section path, whose Switch stub returns
-1, so it threw — even though the surrounding comment says the checked-arena path does not need it.
Horizon needs its own branch that records the arena as one region per request and returns.

### 4.11 `.rodata`-backed structs must not be written
Two output sections contained data that is written at runtime. Moving `.rodata` fixed this generally;
the specific lesson is that any writable-then-read structure (locale tables, vtables) must live in the
writable segment.

### 4.12 Dawn loaded EGL with `dlopen`
`DisplayEGL` tried to load `libEGL.so`, which cannot exist on Horizon: `Failed to load libEGL.so:
"dlopen is not available on Horizon"`. EGL is linked **statically**, so Dawn must use the linked
`eglGetProcAddress` and its existing static "proc and display" initialiser instead.

### 4.13 Two Dawn adapter requirements do not hold on Switch EGL
Mesa's Switch EGL advertises neither `EGL_EXT_create_context_robustness` nor
`EGL_KHR_fence_sync`/`EGL_KHR_reusable_sync`, and Dawn treats both as fatal during adapter discovery.
Relax both under Switch.

### 4.14 A fence-sync fallback was undefined behaviour, and hung silently
With the discovery check relaxed, device creation reached a `DAWN_UNREACHABLE()` used when no fence
extension exists. Undefined behaviour in a release build **hung** rather than reporting anything. Give
it a concrete fallback and creation proceeds. This single change is what made Aurora succeed.

### 4.15 Robustness context attributes were still requested
Adapter discovery passed, but `eglCreateContext` failed with `EGL_BAD_ATTRIBUTE` because the context
was still asked for robustness attributes the extension does not provide. Force them off on Switch.

### 4.16 Linking EGL dragged in Mesa's winsys
Once Dawn referenced EGL directly, the linker pulled in Mesa's `egl_switch.c`, which needs the nouveau
winsys and `fatalThrow` **from libnx**. A failed `fatalThrow` is a missing symbol, not a code bug: the
GL libraries must be followed by the nouveau DRM library and then `libnx` again, because `-lnx` is
scanned too early the first time.

### 4.17 DSP coefficient ROM and NAND paths
- The runtime looks for `dsp_coef.bin` beside the executable, which Horizon has no notion of. It must
  also be searched for on the SD card, and shipped there.
- `nand_root` must be configured, and the value must be a **relative** path. `std::filesystem` treats
  `sdmc:/...` as relative, so an "absolute-looking" value gets joined onto the config directory and
  doubled.
- Put both `dvd_root` and `nand_root` under `[paths]`. A Switch v1.0.2 launch with those keys at
  the top level reached the Wii runtime, fell back to creating a managed NAND, and exited during
  NAND initialization. With `dvd_root = "."` and `nand_root = "NAND"` under `[paths]`, the Switch
  completed an EGL buffer swap and created the 16,288-byte save and 29,344-byte banner.

### 4.18 `pthread_detach` returns ENOSYS and killed the guest
A `std::system_error` with code 88 (newlib `ENOSYS`) escaped from inside a `std::call_once` callable
in the retrace scheduler and became "unhandled C++ exception". The source was `pthread_detach`, which
Horizon does not implement. Treating a failed detach as success removed the exception entirely.

Two measurement traps found while chasing it, both worth remembering:
- **The guest address reported by a throw probe is the last *dispatched* target, not the throw site.**
- The failing `pthread_once` call throws *inside* the callable, so it never returns — every logged call
  reads success even though one of them failed. This is why the hypothesis "pthread_once is fine" was
  simultaneously true of the function and false of the fault.

### 4.19 Uncached memory mirrors were not mapped
`OSInit` reads the running IOS version through the `0xC0000000` MEM1 uncached alias. The arena is a
single 512 MiB block at `0x80000000`, so the alias sat outside it, the read returned nothing, and the
SDK fatally aborted with "This firmware is an improper version for this SDK". Fold
`0xC0000000..0xDFFFFFFF` onto the cached windows — same physical memory, one GiB lower.

### 4.20 The translation registry was never populated
`build-pie` only compiled the `base_common` shards, so the **16 bulk registration shards** were never
compiled and the registry held 396 entries instead of the full map. Entry resolution failed with
"No translated function registered at 0x80004050" even though that address is in the registration
table. Compile them alongside the game shards.

---

## 5. Environment and access

| Item | Value |
|---|---|
| Console | Switch Lite, firmware **20.4.0**, Atmosphère 1.7, modchip |
| Console IP | `192.168.1.111` |
| FTP | port **5000**, served by `sys-ftpd-10k` (not DBI) |
| hbmenu netloader | 28280, single connection — probing consumes it |
| Mac IP | `192.168.1.55` |
| App log channel | the app dials `192.168.1.55:28281` |
| Local emulator | Ryujinx, with the console's keys and firmware |
| Emulated SD | `~/Library/Application Support/Ryujinx/sdcard/`, `sdmc:` maps here |

Sysmodules present: `sys-patch`, `sys-ftpd-10k`, `Tesla`.
Removed because they crash or conflict: `sys-ftpd`, `sys-ftpd-freeze`, `sys-ftp-breeze`, and
**`sys-hidplus`** (a Windows-era build that crashes boot on 20.4.0 — do not re-enable it).

**SD card layout under `sdmc:/switch/aaa-nsmbw/` (v1.0.10 and later):** the config file, `sys/` and `files/` from the extracted
disc, a `NAND/` directory pre-seeded with the WC24 payload, `dsp_coef.bin`, the newest NSP, and the
logs (`boot.log`, `init.log`).

The current hardware-tested installer is
`sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.20-switch-latency-check.nsp`.
The title is `aaa-nsmbw` on HOME. The upgrade keeps the same title ID and moves the existing
NAND and shader cache with the SD directory. Earlier packages expect `sdmc:/switch/nsmbw/`.
On Switch, **A** confirms and **B** goes back in menus and on the world map. In courses, **B**
jumps and **A** runs. The pause menu uses the menu mapping. From v1.0.12, **ZR** is the default
shake button. Existing controller profiles take precedence over these defaults.

Install is **human-only** (ES via DBI or Goldleaf); an agent cannot install an NSP remotely. Installs
take several minutes for the 73 MB package.

---

## 6. Repository layout to recreate

The repo must carry the platform layer and the recipes, because the working tree lives in a scratch
directory that does not survive a reboot.

- `docs/` — this document and the original handoff notes.
- `switch/` — the Switch platform layer: startup override, TLS bootstrap and per-thread attach, SDL3
  subset over libnx, Horizon POSIX shims, Abseil shim, log channel, stub backends.
- `switch/patches/` — the external changes for Aurora, Dawn, and Abseil. Apply
  `aurora-switch-window.patch`, `aurora-switch-vertex-textures.patch`, and
  `aurora-switch-direct-efb-resolve.patch` in the Aurora tree. Apply
  `aurora-switch-controller-defaults.patch` there for the Switch controller defaults.
  For the deko3d GX worker, apply `aurora-switch-gx-worker.patch` in Aurora and
  `runtime-switch-gx-worker.patch` from the runtime checkout root.
  Apply `aurora-switch-overlap-encode.patch` in Aurora after the deko3d profiling patch.
  Apply the Dawn patches in the Dawn tree.
  For the frame profiler, apply `aurora-switch-frame-profile.patch` and
  `aurora-switch-efb-encode-timing.patch` in Aurora, and `runtime-switch-frame-profile.patch` from
  the runtime checkout root. Apply `dawn-switch-gl-timing.patch` after the GLES fence fallback.
- `switch/port/` — a mirror of every *runtime* source file that was modified, with a README naming what
  changed and why.
- Build scripts: full build, PIC-library derivation, relink, incremental single-unit relink, packaging,
  registration-shard compile, single-Dawn-object rebuild, and probe builders.
- Probe programs: tiny NSPs used to reproduce runtime-level problems without the 64 MB package.

---

## 7. Build pipeline order

1. **Full object build** — runtime, Aurora, generated shards, libraries, Abseil, mbedTLS, platform
   layer; position-independent, into one object directory.
2. **Standard runtime libraries** — use the original devkitPro archives with the writable
   `.rodata` linker layout. The old PIC archive variants are useful for historical diagnostics
   (see §4.6).
3. **Registration shards** — compile the 16 bulk registration units (§4.20).
4. **Link** — generate `switch/switch-rw.specs` with `switch/make-rw-ldscript.py`. The incremental
   `switch/rebuild.sh` uses these specs by default, selects RELA relocations, links nouveau DRM and
   libnx after the GL libraries, and wraps the guest thread and exception functions.
5. **Post-link** — patch the dynamic section terminator (§4.4); verify zero read-only relocations.
6. **Package** — convert to NSO, write the NPDM, build the NCA/NSP with a **trimmed** keyset.

Verification that must print zero: read-only relocations. Verification that must print a base-application
title ID.

---

## 8. Test harness

**Ryujinx is the iteration loop.** It can launch the NSP directly and reports emulator faults. The
emulated SD card must contain the same layout as the console for the port to find its input files and
write logs. Confirm each milestone separately on hardware.

For emulator runs, set `NSMBW_LOG_HOST` to an empty string when compiling `switch/log_channel.cpp`
with `switch/rebuild.sh`. The build writes to the emulated SD card without trying the TCP log listener.

Two in-app diagnostics make the port debuggable:

- `init.log` — one line per startup step, written with raw file writes (no stdio) so it survives crashes.
- `boot.log` — the runtime's own log, including Aurora's `[aurora::gpu]` lines, which are the only
  place Dawn/EGL failures are described.

The v1.0.1 startup diagnostic build closes each write to `init.log` before continuing. This lets
another process read the startup trace while the game runs. The standard output redirection keeps
`boot.log` open until the game closes.

Keep traces out of the configuration getter. It runs throughout gameplay, and the repeated
`CFG enter` and `CFG loaded` markers previously grew `init.log` to 134 MB. A fresh instrumented boot
after removing the per-lookup writes produced about 44 KB of startup logs.

### Switch Lite performance

The table shows medians of the slow frames recorded on hardware on 2026-11-19. Boot used 1× in both
runs. The v1.0.4 session changed to 0.5× in the settings menu before the first logged game setup
sample. The effect of the patch on the later scenes at 1× remains unmeasured.

| Scene | Build | Render scale | Whole frame | Before renderer finalization | Renderer finalization |
|---|---|---:|---:|---:|---:|
| Boot (`0x000`) | v1.0.3 | 1× | 271.4 ms | 212.3 ms | 51.8 ms |
| Boot (`0x000`) | v1.0.4 | 1× | 273.6 ms | 221.2 ms | 50.6 ms |
| Game setup (`0x00A`) | v1.0.3 | 1× | 314.9 ms | 259.6 ms | 50.7 ms |
| Game setup (`0x00A`) | v1.0.4 | 0.5× | 270.6 ms | 243.6 ms | 26.7 ms |
| Cutscene (`0x008`) | v1.0.3 | 1× | 314.9 ms | 240.7 ms | 73.4 ms |
| Cutscene (`0x008`) | v1.0.4 | 0.5× | 295.7 ms | 230.5 ms | 66.6 ms |

The direct EFB resolve reads the embedded framebuffer after the draw pass and before the next clear
on OpenGL ES, skipping an intermediate texture copy. In Ryujinx at 1×, the median logged slow frame
in scene `0x005` fell from 88 ms to 22 ms. [Direct EFB resolve patch](../switch/patches/aurora-switch-direct-efb-resolve.patch)

On the Switch Lite, the matched 1× boot samples show nearly the same whole-frame time in both
builds. In the v1.0.4 game setup samples, about 244 ms elapsed before renderer finalization even at
0.5×. The log's `game` field measures that interval in wall-clock time and includes guest execution,
GX calls, and waits. The cause within that interval remains open.

Close the game before retrieving `sdmc:/switch/nsmbw/boot.log` over FTP. The log records up to 30
slow frames per minute. Its two startup watchdog messages counted 26 guest frames at 12 seconds and
354 at 30 seconds on v1.0.4. That works out to 18.2 guest frames per second in that 18-second window.
The cutscene also recorded a 427.9 ms frame with zero disc reads. The sampled slow-frame medians
describe stalls. A sustained gameplay FPS average needs a continuous frame count over the same course.

**Tiny probe NSPs** are the fastest tool in the kit: a 300 KB package that reproduces one runtime
behaviour (config load, `<iostream>`, thread-local, EGL surface) in ~40 seconds. Several of the hardest
bugs in §4 were found this way rather than through the 64 MB game package.

### Frame profiler

Use handheld mode, 1× resolution and the standard Switch VSync mode in a separate Ryujinx profile.
This matches the guest's display and input mode. Ryujinx runs the graphics workload through the
host GPU, so use the Switch Lite to measure the hardware's frame times.

With v1.0.5 or later, create an empty `sdmc:/switch/nsmbw/frame-profile.flag` before launching.
Play a repeatable scene or course at 1× and close the game. In v1.0.6 the background writer batches
frame reports and presentation hitch messages into `sdmc:/switch/nsmbw/frame.log`. Startup and error
diagnostics remain in `boot.log`. The `fps` lines report two-second averages and the worst frame,
with elapsed times for GX processing, renderer finalization, and pacing. The following lines report
guest scheduler waits and audio activity.

`Switch frame profile` reports how much time went into preparing the next frame and how long the
presenting host thread was scheduled on a CPU. The `sampled` count indicates how many frame intervals
had valid CPU ticks. This timing covers the presenting thread. Audio and other worker threads have
their own counters. To disable the measurements for subsequent launches, remove the flag file.

The v1.0.5 hardware run at 1× measured a median of 7.3 FPS across the stage's two-second windows.
Renderer finalization took about 40 ms per frame, including about 37 ms of active CPU time on the
presenting thread. v1.0.6 moved profiler writes off that thread. On the Lite, the matching stage's
median improved to 12.7 FPS. The final cutscene still averaged 11.1 FPS over its last 30 seconds,
with about 39 ms before renderer finalization and 51 ms in finalization per frame.

### v1.0.7 Switch Lite renderer comparison

Keep the stock clocks, 1× resolution, and the same save and scene for both runs. Play the same
section for at least 30 seconds in each build, then close it from the HOME menu to release the
files for FTP. Compare the final 30 seconds of that section, using the two-second FPS windows and
their reported worst frame times.

| Package | Renderer | Logs under `sdmc:/switch/nsmbw/` |
|---|---|---|
| `nsmbw-v1.0.7-a-switch-gles-timing.nsp` | Mesa OpenGL ES | `boot.log`, `frame.log` |
| `nsmbw-v1.0.7-b-switch-deko3d.nsp` | Dawn/deko3d | `boot-deko3d.log`, `frame-deko3d.log` |

Build A reports wall time spent replaying GL commands and waiting in the queue's `glFinish`
fallback. Both report CPU time spent encoding EFB copies. These counters can overlap the other
frame timings and should not be added to them. Build B also changes the Dawn revision, uses
storage-buffer vertex fetch, and keeps Aurora's EFB snapshots, so this comparison measures the
two renderer configurations as a whole.

Build B uses [Dawn at `13abc3bc`](https://dawn.googlesource.com/dawn/+/13abc3bc8ea2d3c2050f9e77a12d012108ceee24),
[the Switch backend patch](https://github.com/schuler-ph/nsmbw-switch/blob/502d87fc10111017dbaeb354738bcd4f2f8474db/aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch),
[the Deko3d and libuam compatibility patches](https://github.com/schuler-ph/nsmbw-switch/tree/502d87fc10111017dbaeb354738bcd4f2f8474db/projects/nsmbw/tools/patches),
and [libuam at `f8c9eef0`](https://github.com/averne/libuam/tree/f8c9eef01ffe06334d530393d636d69e2b52744b).
The dependency build uses devkitA64, libnx, libdeko3d, and the
[Switch dependency recipe](https://github.com/schuler-ph/nsmbw-switch/blob/502d87fc10111017dbaeb354738bcd4f2f8474db/projects/nsmbw/tools/build-switch-deps.sh).
Build Dawn as a PIC static library with deko3d enabled and the GL backends disabled. Enable the
Tint WGSL reader and the WGSL and GLSL writers. The libnx portability fixes used in this build
are pinned in [the WiiCompiled dependency recipe](https://github.com/yashin-sh/WiiCompiled-Switch/blob/b851db949cc9764c306877ed749bc70800f9658f/scripts/prepare-native-deko3d-deps.sh).

Use separate scratch copies of the GLES object tree and packaging directory. Apply
`aurora-switch-deko3d.patch` after the other Aurora patches, and use the runtime `main.cpp` mirror.
Mount the dependency directory at `/deko` with Dawn source in `dawn/`, its CMake output in
`dawn-build/`, and `libuam.a` in `uam/build-switch/`. Set `NSMBW_SWITCH_BACKEND=deko3d` and
recompile every Aurora unit, runtime `main.cpp`, `switch/thread_tls.cpp`, and
`switch/log_channel.cpp` with `switch/rebuild.sh --compile-only`. Recompile ImGui's WebGPU backend
with the same Dawn headers before relinking with `--pack`. Set `NSMBW_LOG_HOST=` for SD logging.
Build B gives default pthreads 1 MiB of stack for the Deko shader compiler.

### v1.0.8 shader startup

On the same Lite, Build B improved the sampled world-map median from 10.5 to 16.8 FPS, and
its fastest window reached 34.5 FPS. The final stage still averaged 1.75 FPS over its last 30
seconds, versus 1.64 FPS on Build A. Build B recorded a 27.6-second renderer frame when the
world map first appeared with nine pipelines queued. The pipeline database could not be
opened, and both builds wrote repeated GX debug diagnostics synchronously during the stage.

v1.0.8 keeps deko3d and 1×. It filters debug messages on Switch, enables the existing Dawn
cache callbacks, and warms known GX pipelines on the existing **Loading shaders** screen.
The game can still encounter new GX configurations during later play, which are learned for
the next launch. The screen waits up to three minutes before it lets startup continue.
In Ryujinx, the empty-cache pass warmed all 1,816 bundled pipelines in 106.6 seconds. The
reopen test on v1.0.8-r1 warmed the same pipelines in 2.8 seconds and hit all 4,740 blob-cache
lookups. These measurements cover startup in the emulator.
The seed comes from `runtime/assets/pipeline/initial_pipeline_cache.db`; copy it to the root
of `/pack/romfs` and rebuild the NSP with the `1.0.8-r1` NACP display version. The two writable
databases live in `sdmc:/switch/nsmbw/Cache/deko3d/`.

Place [the pinned libnx SQLite VFS](https://github.com/nx-mod/sqlite-nx/blob/77103483f20795f744044f931596e7f4cc7858e0/source/nx-vfs.c)
at `/deps/sqlite/nx-vfs.c` with the SQLite 3.46.1 amalgamation. Apply
`sqlite-nx-ipc-path.patch` in that dependency directory so libnx receives its full-size
IPC pathname buffer on cache reopen. Run
`bash switch/build-sqlite-cache.sh` in the deko3d build container. Apply
`aurora-deko3d-shader-cache.patch` after `aurora-switch-deko3d.patch` and
`runtime-switch-shader-warmup.patch` from the runtime checkout root. Recompile the patched units,
runtime `main.cpp`, and `switch/switch_appinit.cpp` before packaging.

On the Switch Lite, the first cold boot reached 406 of 1,816 pipelines by the three-minute
cutoff. Merging the same build's precompiled Dawn cache added 2,909 blobs and preserved all
1,561 blobs already compiled on the Lite. The next launch warmed all 1,816 pipelines in 23.2
seconds with 4,740 hits out of 4,740 cache lookups.

| Warm-cache scene at 1× | Median FPS | Median frame time | Median main-thread CPU time before renderer finalization |
|---|---:|---:|---:|
| Playable stage (`0x005`, 121 windows) | 24.2 | 41.4 ms | 26.1 ms |
| World map after the stage (`0x003`, 12 windows) | 19.1 | 52.4 ms | 36.7 ms |

The final 30 seconds of the playable stage averaged 26.1 FPS. The frame profiler measures
the game thread and the elapsed time of its finalization call. The deko3d render and present
workers run asynchronously, so their CPU work and GPU completion require separate timings.

### v1.0.9 handheld timing

v1.0.9 uses the existing deko3d shader databases. Keep `frame-profile.flag` on the SD card and
test at 1× with stock handheld clocks. Run the same world-map section and the same playable
course for at least 30 seconds each, then close the software before pulling
`boot-deko3d.log` and `frame-deko3d.log`.

- `frame p95` is the upper edge of a quarter-millisecond bucket for each two-second sample window.
  The last bucket covers frames of at least 128 ms.
- `handheld profile` reports elapsed time in producer waits for the frame worker, worker encoding,
  staging maps, and synchronous EFB-to-RAM mapping and encoding. Those timings can overlap.
- `deko3d profile` reports elapsed CPU fence waits per frame and the average GPU marker interval
  per sampled command list. It samples roughly one in eight lists and reads the reports after
  the existing completion fence. The GPU value describes one list.

For the build, apply `aurora-switch-deko3d-profile.patch` in Aurora after the other Switch patches,
`runtime-switch-frame-percentiles.patch` from the runtime checkout root, and
`dawn-deko3d-submit-profile.patch` in the deko3d Dawn tree. Build the Dawn archive and recompile the
patched Aurora and runtime units plus `switch/log_channel.cpp`. Set the NACP display version to
`1.0.9` and keep the pipeline seed in RomFS when packaging.

### v1.0.13 GX worker test

The test installer is `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.13-switch-gx-worker.nsp`.

On deko3d, the GX worker checks the process's allowed Horizon cores because devkitA64's
`std::thread::hardware_concurrency()` reports zero. The runtime snapshots mutable matrix data
using the same worker state as Aurora. Recompile Aurora's `lib/gx/fifo.cpp` and the runtime's
`gx_dl.cpp`, `gx_fifo.cpp`, and `gx_vertex.cpp` in `runtime/src/hle/gx/`.

Compare the same course and world-map route for at least 30 seconds each at stock handheld clocks.
The boot log should report `GX thread started (Switch)`. Compare the game thread's scheduled CPU
time, frame p95, and sustained FPS against v1.0.11 with the same resolution and warm shader cache.

### v1.0.14 overlapping render encoding test

The test installer is `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.14-switch-overlap-encode.nsp`.

With v1.0.13, comparable world-map samples with roughly 11,000–12,000 GX draws per frame
rose from a median 19.1 to 22.9 FPS. The median game-thread CPU time fell from 37.8 to
26.7 ms per frame, and render-worker waits remained near 15 ms. The final 30 seconds of
course play averaged 23.7 FPS in v1.0.13. That section had substantially more GX draws
than the final 30 seconds of the earlier course capture.

v1.0.14 retains the GX worker and allows next-frame GX processing to overlap encoding
of the previous frame. Recompile Aurora's `lib/aurora.cpp` and `switch/sdl_switch.cpp`.
The SDL shim returns the Switch's single display to ImGui's deferred frame setup. Check for
`Switch render encoding: overlapping` in the boot log and compare the same map and course
sections against v1.0.13 at the same resolution and stock handheld clocks.

On the Switch Lite at 1× and stock handheld clocks, the median two-second sample improved
from 22.9 to 36.4 FPS on the world map (10,500–12,600 GX draws per frame) and from 23.5 to
35.4 FPS in heavy course sections (6,900–8,200 draws). Median frame-worker wait fell from
14.94 to 0.06 ms on the map and from 14.46 to 0.08 ms in those course sections. The game
thread still used 26–27 ms per frame in both heavy workloads, and one map frame took
2.74 seconds. These results confirm the improvement and leave a gap to sustained 60 FPS.

### v1.0.15 graphics worker affinity test

The test installer is `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.15-switch-core-affinity.nsp`.

With all three application cores available, pin GX decoding to core 1 and frame encoding
to core 2. The boot log records both affinity results. Apply
`aurora-switch-worker-affinity.patch` after the GX worker and overlapping encode patches.
Recompile Aurora's `lib/aurora.cpp`, `lib/gx/fifo.cpp`, `lib/gfx/pipeline_cache.cpp` and
`switch/log_channel.cpp`. With `frame-profile.flag` enabled, `GX profile` measures FIFO
synchronization and pipeline waits separately to diagnose spikes.

Compare the same map and course sections at 1× with warm caches and stock handheld clocks.
The related Switch port queues texture copies on the GX worker. That experiment produced
visible corruption in this build in Ryujinx, so this test uses the copy ordering from v1.0.14.
See [the related port's texture copy change](https://github.com/schuler-ph/nsmbw-switch/commit/4e3a98be14f2).

The Lite confirmed both affinity assignments. The final 30.5 seconds of course play
averaged 42.3 FPS with 22.6 ms of game-thread CPU per frame. The last 12.1 seconds recorded
on the map averaged 38.5 FPS. The heavier map windows still took a median 26.5 ms of
game-thread CPU per frame. No pipeline wait was measurable in this capture.

### v1.0.16 vertex state reuse test

The test installer is `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.16-switch-vertex-state.nsp`.

Apply `aurora-switch-vertex-state.patch` in Aurora and `runtime-switch-vertex-state.patch`
from the runtime checkout root after the GX worker patches. Recompile Aurora's
`lib/dolphin/gx/GXGeometry.cpp` and the runtime's `runtime/src/hle/gx/gx_fifo.cpp` and
`runtime/src/hle/gx/gx_vertex.cpp`.

Reuse the vertex layout and register shadow for consecutive buffered draws when the
runtime layout generation and Aurora's producer registers still match. The same register
writes are emitted in FIFO order. Direct FIFO stores that fit in a pending packet go
straight into it. The first successful reuse logs `Switch GX vertex-state reuse active`.
Compare the same map and course at 1× and stock handheld clocks, watching for visual
differences as well as timing changes. The register-shadow approach is adapted from
[the related Switch port](https://github.com/schuler-ph/nsmbw-switch/commit/ee154ddf8a21).

The final 30.2 seconds of course play averaged 48.5 FPS, up from 42.3 FPS over the final 30.5
seconds on v1.0.15. Across 60 course windows, median scheduled game-thread CPU was 20.27 ms per
frame. Median waits for the frame worker and GX FIFO were 0.09 ms and 0.36 ms, respectively.
Pipeline waits rounded to 0.00 ms and sampled GPU lists took a median 10.17 ms. The GPU list
sample covers individual lists. It does not measure whole-frame GPU time or utilization.

Among world map windows with 10,500–12,600 recorded GX draws, median FPS was 40.2 on v1.0.16
and 35.9 on v1.0.15. Median scheduled game-thread time was 23.82 ms and 26.27 ms, respectively.
These groups contain five and twelve two-second windows. Two large hitches included 80.22 ms
of disc reads in a 320.4 ms course frame and 190.97 ms in a 364.6 ms map frame.

### v1.0.17 checked memory and disc cache test

The installer is `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.17-switch-checked-ram.nsp`.
Apply `runtime-switch-checked-ram.patch` and `runtime-switch-disc-cache.patch` from the runtime
checkout root after the preceding patches. Use the updated `switch/rebuild.sh` to rebuild all
translated game shards and the runtime units with inlined checked memory accesses:

```sh
bash switch/rebuild.sh --translated --pack \
  runtime/src/guest_flat_memory.cpp runtime/src/memory.cpp runtime/src/ppc_quantized.cpp \
  runtime/src/hle/egg_decomp.cpp runtime/src/hle/storage/dvd.cpp
```

Horizon always uses checked guest memory access, so the translator's C++ can fold that branch
at compile time. Scalar stores reject coarse-page crossings with a single page-offset check.
Executable-write, MMIO and deferred-read policy still use the same checked paths.

The DVD layer keeps up to 16 native read-only files open. Repeated reads skip file open and
size lookup. A native failure falls back to the existing read path. The first successful reuse
logs `Switch DVD file cache active`. Compare the same course and map at 1× and stock clocks,
checking median scheduled CPU, FPS, frame-time percentiles and disc time on hitch frames.
Ryujinx at 1× reached the world map and World 1-1, and the boot log confirmed the DVD cache
was active. The Lite's v1.0.17 run confirmed the descriptor cache was active. The recorded
26.2 seconds of the manual course averaged 44.1 FPS with a median 19.22 ms of scheduled
game-thread CPU per frame. The previous course capture covered a different stretch. The final
4 seconds on the map averaged 42.0 FPS and included a 345.3 ms frame. The startup capture
still recorded 368 ms across 72 reads in one frame. The profile's slow-frame limit means the
largest frame in a window does not always have a disc timing breakdown.

### v1.0.18 checked memory ranges, disc cache and profiling

Installer: `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.18-switch-memory-and-io.nsp`.
Apply `runtime-switch-memory-and-io.patch` from the runtime checkout root,
`aurora-switch-gpu-frame-profile.patch` from Aurora, and `dawn-deko3d-frame-profile.patch`
from Dawn after `dawn-deko3d-submit-profile.patch`. Copy the updated Switch platform files
and rebuild Dawn's device object before linking the NSP. For the deko3d container layout:

```sh
DAWN_BUILD_DIR=/deko/dawn-build \
  DAWN_AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar \
  python3 switch/rebuild-dawn-object.py DeviceDeko3d.cpp
bash switch/rebuild.sh --translated --pack switch/log_channel.cpp \
  runtime/src/hle/storage/dvd.cpp runtime/src/hle/gx/gx_vertex.cpp aurora-main/lib/aurora.cpp
```

Translated access groups can reuse a RAM pointer when the existing memory tables prove the whole
group fits in a single mapped 1 MiB page and satisfies its read and write policy. Cross-page ranges,
deferred reads and executable writes retain the checked fallback.

The DVD layer retains successful whole-file reads of at most 128 KiB within an 8 MiB budget.
This reuses data already requested by the game. With profiling enabled, `frame-deko3d.log`
records filenames for up to 96 reads of at least 8 ms to identify further prefetch candidates.

The same profile estimates GXBegin time from sampled calls and pairs render and composition
GPU timestamps by logical frame once every 16 frames. The combined render and composition
duration covers the two measured command batches. The elapsed span also includes the gap
between them. Earlier copy-only work, queue work outside those batches and GPU utilization
are outside this measurement. The profile reads timestamps after the existing fence completes.
Ryujinx at 1× reached World 1-2 and returned to the map. Its logs confirmed RAM cache hits,
GX producer timings, and paired GPU samples.

On the Lite, the recorded 106.7-second course averaged 43.9 FPS and the final 30.2 seconds
averaged 47.2 FPS. The final 6.0 seconds on the map averaged 44.4 FPS. In the slowest
two-second course window (24.9 FPS), game-thread CPU was 17.85 ms, worker wait was 21.03 ms
per frame and staging map time was 4.57 ms per frame. In the final window at 58.9 FPS, those
readings were 12.42, 0.53 and 0.29 ms. The worker wait aggregates callers and can overlap
the staging timer. Several long course frames had no disc I/O. GPU pair coverage was
sparse in the slow windows. The capture is a different length and workload from v1.0.17.

### v1.0.19 render staging and HOME icon

Installer: `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.19-switch-render-staging.nsp`.
Apply `dawn-deko3d-host-upload-sources.patch` in Dawn after the preceding Dawn patches.
Apply `aurora-switch-worker-wait-phases.patch` in Aurora, copy the updated Switch platform files,
and rebuild the two Dawn objects and the Switch units:

```sh
export DAWN_BUILD_DIR=/deko/dawn-build
export DAWN_AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar
python3 switch/rebuild-dawn-object.py BufferDeko3d.cpp
python3 switch/rebuild-dawn-object.py CommandBufferDeko3d.cpp
bash switch/rebuild.sh --pack aurora-main/lib/aurora.cpp switch/log_channel.cpp
```

Before packaging, set the NACP display version to 1.0.19 and place the supplied HOME artwork
in `/pack/control_g/icon_AmericanEnglish.dat` as an RGB 256×256 JPEG without EXIF. The
installer's control NCA contains the artwork supplied for this build. It is local to the package.

The deko3d backend copies host-only upload sources while recording, into inline command data
or separately owned upload memory. These sources now use CPU-cached memory with no GPU mapping.
Their next map no longer waits for a GPU serial from that copy. Buffers used by the GPU continue
to use normal serial tracking. The profile reports waits for preparation and completion
separately, and it includes GPU samples whose render and composition intervals overlap.
Watch for a lower staging-map wait, fewer long frames and correct textures in the same course.
Ryujinx logs confirmed the upload path and the new wait counters were active through the
world map and a manually entered course.

The Lite's v1.0.19 capture reported effectively zero staging-map wait in the course windows.
Near the end of the run, a 2,481 ms frame coincided with a 2,477 ms presentation surface
acquire. The player reported over a second of latency. The capture does not establish whether
the stall happened while playing or when the player left the game, and it does not measure
button-to-screen latency.

### v1.0.20 staging rollback and presentation timing

Installer: `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.20-switch-latency-check.nsp`.
Use Dawn from v1.0.18, with the paired GPU profile patch. Omit
`dawn-deko3d-host-upload-sources.patch`; if it was applied, reverse it and rebuild
`BufferDeko3d.cpp` and `CommandBufferDeko3d.cpp` before linking. This restores the
previous upload mapping and source serial tracking for the latency comparison.

Keep the v1.0.19 worker-phase profile and supplied icon. Apply
`aurora-switch-presentation-latency.patch` in Aurora, copy the updated Switch platform
files, set NACP version 1.0.20 and run:

```sh
bash switch/rebuild.sh --pack aurora-main/lib/aurora.cpp switch/log_channel.cpp
```

With profiling enabled, the log reports the peak delay between enqueue and start of
presentation and the peak time spent acquiring a display buffer for jobs that reached
acquire. Compare a continuous course with the same controller and display settings,
including time spent playing before leaving the game. These timings describe the display
pipeline and cannot directly measure button-to-screen delay. Ryujinx confirmed the new timings
through the world map.

The Lite recorded 161.1 seconds of course play at 46.2 FPS overall, including 52.1 FPS in
the final 30.2 seconds. In the 29 two-second course windows below 45 FPS, median game-thread
CPU time per frame was 22.66 ms and the median sampled render-plus-composition duration was
17.36 ms. These windows had a median 5,936 GX draws and 30 framebuffer copies per frame.
Peak presentation queue and image acquisition waits for the course were 1.27 and 1.87 ms.
The 2.48-second acquisition stall from v1.0.19 did not recur in this capture.

### v1.0.21 draw state reuse

Installer: `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.21-switch-draw-state.nsp`.
Use the v1.0.20 build, including the supplied HOME icon, and set NACP version to 1.0.21.
Apply `dawn-deko3d-reuse-draw-state.patch` and copy the updated Switch platform files:

```sh
DAWN_BUILD_DIR=/deko/dawn-build \
  DAWN_AR=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-ar \
  python3 switch/rebuild-dawn-object.py CommandBufferDeko3d.cpp
bash switch/rebuild.sh --pack aurora-main/lib/aurora.cpp switch/log_channel.cpp
```

deko3d now compares index addresses, index formats and primitive-restart state across
consecutive indexed draws in a render pass. It also skips dirtying texture units whose
descriptor inputs are identical within a pipeline. Draw order, framebuffer copies and
render settings are preserved. The runtime reports saved index and restart commands and
unchanged texture inputs in the frame log.

`frame-profile.flag` still enables FPS, CPU, renderer and presentation timing. Create
`gpu-frame-profile.flag` alongside it before launch to enable tiled-cache-flushing GPU
timestamps. Use the same course and map at stock clocks and 1× to check both responsiveness
and the frequency of long frames. Ryujinx reached the map and a course with GPU timestamps
off. One map window avoided a mean 549 index and restart commands per frame.

The Switch Lite averaged 47.5 FPS over the first course run (145 seconds), including
59.9 FPS over its final 30 seconds. A second course averaged 44.9 FPS over 75 seconds
and 44.3 FPS over its final 30 seconds. The latter section had a median 30 framebuffer
copies per frame and 3.47 ms per frame spent waiting for renderer preparation. The logged
presentation queue and image acquisition peaks for that section were below 0.8 ms.

### v1.0.22 direct framebuffer resolves

Install `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.22-switch-direct-efb.nsp`.
Use the v1.0.21 build, including the HOME icon. Set the NACP version to 1.0.22, apply
`aurora-switch-deko3d-direct-efb.patch` in the Aurora tree and copy the updated Switch files.
Reuse the v1.0.21 Dawn archive.

```sh
bash switch/rebuild.sh --pack aurora-main/lib/gfx/common.cpp switch/log_channel.cpp
```

For eligible color resolves, deko3d reads the main single-sampled Wii framebuffer directly
after the draw pass and before the clear. The destination, copy filter and 1× render scale
stay the same. Offscreen targets, multisampled targets and one-shot texture bakes keep
their existing copy paths.

Ryujinx displayed the map and World 1-1 at 1×. Steady course windows logged up to 25 skipped
snapshots per frame out of 31 resolves. The `frame-profile.flag` report records skipped and
retained snapshots per frame. To compare the two paths in the same build, create an empty
`sdmc:/switch/aaa-nsmbw/efb-snapshot.flag` and relaunch to restore the old snapshot path.

The Switch Lite averaged 53.7 FPS across the 74.6-second course capture at 1×. The final
30 seconds before the return to the map averaged 54.2 FPS (18.44 ms per frame). Both final
sections below had a median of 30 GX copies and about 2,300 GX draws per frame.

| Last 30 seconds of course play | v1.0.21 second course | v1.0.22 |
| --- | ---: | ---: |
| Average FPS | 44.3 | 54.2 |
| Two-second FPS range | 37.2–54.8 | 47.9–60.1 |
| Two-second windows with a frame over 50 ms | 14 of 15 | 2 of 15 |
| Longest frame | 318.7 ms | 73.8 ms |
| Median renderer preparation wait per frame | 3.47 ms | 0.07 ms |

The new path skipped a median of 25 snapshots per frame. In the slowest final window,
the game thread was active for 19.35 ms per frame with 0.07 ms spent waiting for renderer
preparation. That points to reducing active CPU work to reach the 16.67 ms budget more
consistently. This comparison uses separate play runs. A test with the v1.0.22 fallback
flag would isolate the effect of the snapshot change.

### v1.0.23 earlier GX worker handoff

Install `sdmc:/switch/aaa-nsmbw/aaa-nsmbw-v1.0.23-switch-gx-handoff.nsp`.
Use the v1.0.22 build and HOME icon, set the NACP version to 1.0.23, apply
`aurora-switch-gx-early-handoff.patch` in Aurora and copy the updated Switch files.

```sh
bash switch/rebuild.sh --pack aurora-main/lib/gx/fifo.cpp switch/log_channel.cpp
```

The game thread hands buffered GX commands to the worker at 4 KiB so decoding can start
earlier. The decoder still consumes batches in FIFO order, and the game thread still
waits at the existing synchronization points. The threshold is checked at the same safe
command boundaries. Direct framebuffer resolves and the 1× render scale carry over from v1.0.22.

The `GX batch profile` in `frame-deko3d.log` reports batch count, time spent handing off
batches, and elapsed time in inline decoding on the game thread. Compare these with
game-thread active time, FIFO synchronization wait and frame times. For a controlled
comparison in the same build, create an empty `sdmc:/switch/aaa-nsmbw/gx-fifo-16k.flag`
and relaunch to restore the 16 KiB threshold. The boot log confirms the active threshold.

Ryujinx displayed World 1-1 at 1× with the 4 KiB threshold. The new counters recorded
26–36 worker batches per frame and 0.43–0.45 ms of inline decoding in sample course
windows.

The Lite capture confirmed the 4 KiB threshold and 1× render scale. The two played
courses averaged 54.5 FPS over 62.4 seconds and 55.5 FPS over 86.5 seconds.

| Last 30 seconds of course play | v1.0.22 | v1.0.23 first course | v1.0.23 second course |
| --- | ---: | ---: | ---: |
| Average FPS | 54.2 | 54.9 | 57.7 |
| Lowest two-second average | 47.9 FPS | 47.4 FPS | 42.9 FPS |
| Frames over 20 ms | 18.8% | 16.5% | 3.8% |
| Longest frame | 73.8 ms | 82.1 ms | 112.4 ms |
| Median FIFO synchronization wait per frame | 0.37 ms | 0.00 ms | 0.02 ms |

The last seven two-second windows of the second course averaged 60 FPS. Its final
30 seconds issued fewer GX draws than v1.0.22, with medians of 1,733 and 2,296 draws
per frame. Across course windows restricted to 1,800–3,400 draws and 24–31 copies per
frame, FPS ranged from 52.6–53.8 across the two v1.0.23 runs, versus 53.2 on v1.0.22.
The 16 KiB same-build comparison is still pending.

The slowest full-course window on v1.0.23 averaged 36.6 FPS with 25.71 ms of game-thread
CPU time, 0.39 ms of FIFO wait and 0.07 ms of renderer preparation wait per frame.
The game thread still exceeds the 16.67 ms budget in busy scenes.

---

## 9. Pitfalls that cost the most time

- **`readelf -SW` right-aligns the section index**, so the section name is field 2 for some objects and
  field 3 for others. Extract names by pattern, never by position.
- **`ar r` does not rebuild the symbol index.** Without a follow-up index regeneration the linker keeps
  using the old object, so a "fix" appears to have no effect.
- **Passing a relative object path to `ar` from the wrong directory silently does nothing.**
- **A failed compile leaves the old object in place**, so the next run measures the previous binary.
  Verify the new symbol is present in the archive before trusting a result.
- **`extern "C"` declarations cannot live inside a function.**
- **Dawn's include path carries no libnx headers**; forward-declare what a patch needs.
- **Inline functions in headers are subject to COMDAT selection**, so instrumenting one in a header may
  have no effect unless every translation unit is rebuilt — or unless a different copy wins.
- **`--wrap` is the safe way to instrument** library calls (Bay, thread APIs, exceptions) without
  duplicate-definition errors.
- **The log channel's connect retry** burned ~50 s per run when no listener was up, which made healthy
  runs look hung and led to killing them mid-startup.
- **A `DAWN_UNREACHABLE()` in release code hangs** rather than aborting.
- **Never treat a "no change" result as a disproof** until you have confirmed the new code is actually
  in the binary.

---

## 10. Remaining work on Switch Lite

1. Measure continuous FPS and frame-time percentiles on a repeatable course at 1×. Time guest execution,
   GX and driver calls, and scheduler waits separately to find the cause of the long frames.
2. Verify a full level run, audible output, and save reload across a restart on hardware.
3. Finish NSP metadata and packaging polish.

---

## 11. Decisions taken, and open questions

**Decided (2026-11-19):**

* **Acceptance bar is full playability** — menus, input, audio, and levels that can be played. A first
  frame is a milestone, not the finish line.
* **This repository is the long-term home**, on the branch `switch-playable`, created from `switch-port`
  for the push to full playability. The platform layer is not being folded into the runtime's own tree.
* **Validate native performance on the Switch Lite.** Ryujinx is useful for testing rendering and
  input changes. Compare frame timings on the console before judging playability.
* **Game data and console keys never enter the repository** — see §0.

**Open:**

1. **If work resumes:** fix one blocker and report each time, or chain several before reporting? Not yet
   decided.
