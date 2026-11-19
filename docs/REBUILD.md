# NSMBW on Nintendo Switch — full project state and rebuild guide

**Purpose of this document.** Everything needed to understand, recreate, or continue this project from
nothing, without the working tree. It records what the goal is, what has been proven, every platform
defect found and how it was defeated, the exact environment, and the single blocker in flight.

Status at time of writing: **the port builds, installs, launches, initialises graphics, runs the game's
own code — but has never presented a frame.** One defect remains between here and a first frame.

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

"Done" means: installs from the home menu, boots, and the game itself is playable.

---

## 2. The blocker in flight (start here)

Surface creation fails, so nothing is ever drawn and Ryujinx keeps showing its loading splash.

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

**The fix being applied when work stopped** — build the window explicitly instead of trusting the
default:

1. In `__appInit`, after `appletInitialize()`: initialise VI as the application.
2. Open the default display and create a VI layer on it.
3. Create an NWindow from that layer, store it once in a global.
4. Have **both** the SDL platform layer and Aurora's surface descriptor use that handle, falling back
   to the default only if creation failed.

This compiled once in an earlier form; the last edit (moving an `extern "C"` declaration to file scope,
which is required — it cannot live inside a function) was not yet verified by a run. **Verify that
first.** If `valid=1` appears and `eglCreateWindowSurface` succeeds, the next milestone is a presented
frame.

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
- All **184** DOL static constructors execute.
- The translated-function registry holds the full map (not the truncated 396 entry subset).
- The guest executes translated PowerPC code and dispatches indirectly.
- **Aurora + Dawn + Mesa EGL initialise successfully**: an OpenGL ES 3.2 adapter is found and the
  WebGPU device is created (`aurora status=0`), with the renderer reaching swapchain setup.
- Zero unhandled C++ exceptions in the current build.

What is *not* proven: a presented frame, audio, input, the guest main loop.

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

### 4.3 `npdmtool` silently truncates short service names
An NPDM granting `hid` or `sm:` loses those entries because the tool drops names shorter than four
characters, and the `"*"` wildcard produces an empty table. libnx's default `__appInit` then aborts at
`smInitialize`. **Fix: override the weak `__appInit`** and initialise only what exists, ignoring
failures. Do not use stdio inside it — it runs inside CRT before streams exist.

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

**SD card layout under `sdmc:/switch/nsmbw/`:** the config file, `sys/` and `files/` from the extracted
disc, a `NAND/` directory pre-seeded with the WC24 payload, `dsp_coef.bin`, the newest NSP, and the
logs (`boot.log`, `init.log`).

Install is **human-only** (ES via DBI or Goldleaf); an agent cannot install an NSP remotely. Installs
take several minutes for a ~64–69 MB package.

---

## 6. Repository layout to recreate

The repo must carry the platform layer and the recipes, because the working tree lives in a scratch
directory that does not survive a reboot.

- `docs/` — this document and the original handoff notes.
- `switch/` — the Switch platform layer: startup override, TLS bootstrap and per-thread attach, SDL3
  subset over libnx, Horizon POSIX shims, Abseil shim, log channel, stub backends.
- `switch/patches/` — the Dawn and Abseil edits as instructions, since those live outside the repo.
- `switch/port/` — a mirror of every *runtime* source file that was modified, with a README naming what
  changed and why.
- Build scripts: full build, PIC-library derivation, relink, incremental single-unit relink, packaging,
  registration-shard compile, single-Dawn-object rebuild, and probe builders.
- Probe programs: tiny NSPs used to reproduce runtime-level problems without the 64 MB package.

---

## 7. Build pipeline order

1. **Full object build** — runtime, Aurora, generated shards, libraries, Abseil, mbedTLS, platform
   layer; position-independent, into one object directory.
2. **PIC libraries** — surgically rename only relocation-targeted `.rodata` sections (see §4.6). With
   §4.5 working this step becomes optional.
3. **Registration shards** — compile the 16 bulk registration units (§4.20).
4. **Link** — custom specs file pointing at the modified linker script; RELA rather than RELR; GL
   libraries followed by nouveau DRM and libnx again (§4.16); wrap flags for the guest-thread and
   exception instrumentation.
5. **Post-link** — patch the dynamic section terminator (§4.4); verify zero read-only relocations.
6. **Package** — convert to NSO, write the NPDM, build the NCA/NSP with a **trimmed** keyset.

Verification that must print zero: read-only relocations. Verification that must print a base-application
title ID.

---

## 8. Test harness

**Ryujinx is the iteration loop.** It reproduces the console exactly, needs no install, and gives stack
traces. The emulated SD card is a real directory and must contain the same layout as the console, or the
app's first `fopen` fails silently and there are no logs at all.

Two in-app diagnostics make the port debuggable:

- `init.log` — one line per startup step, written with raw file writes (no stdio) so it survives crashes.
- `boot.log` — the runtime's own log, including Aurora's `[aurora::gpu]` lines, which are the only
  place Dawn/EGL failures are described.

**Tiny probe NSPs** are the fastest tool in the kit: a 300 KB package that reproduces one runtime
behaviour (config load, `<iostream>`, thread-local, EGL surface) in ~40 seconds. Several of the hardest
bugs in §4 were found this way rather than through the 64 MB game package.

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

## 10. Remaining work after the surface

1. Present a frame (the immediate milestone).
2. Audio — `audren` initialisation and the DSP mix path.
3. Input — `hid` is still blocked for the *NPDM* by §4.3, which may need a different NPDM writer.
4. Guest main loop, then actual gameplay.
5. NSP polish: icon, version string, replacing the placeholder ticket.

---

## 11. Decisions taken, and open questions

**Decided (2026-11-19):**

* **Work is paused.** Nobody resumes this automatically; it is handed off via this document.
* **Ryujinx is the only validation environment for now.** The surface fix does not need console
  validation before continuing.
* **Game data and console keys never enter the repository** — see §0.

**Open:**

1. **Where should this live long-term?** Keep the platform layer in this repo, or fold it into the
   runtime's own tree as a first-class target?
2. **Is a first frame the acceptance bar**, or full playability (level select, controls, audio)? This
   was not answered and is still unknown.
3. **If work resumes:** fix one blocker and report each time, or chain several before reporting?
