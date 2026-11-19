# Switch port — state and handoff

Status: **the NSP now loads and executes C++ code on hardware.** `sdmc:/switch/nsmbw/boot.log`
contains the line written by the preinit hook before any static initialiser runs:

```
nsmbw: reached C++ runtime (log
```

That means the NCA/NSP packaging, the NPDM, and the loader/relocation path are all correct. The app
still does not reach the game; it dies shortly after C++ startup. Everything below is what it took to
get here, so none of it has to be rediscovered.

---

## 1. Console / environment

| Item | Value |
|---|---|
| Console | Switch Lite, firmware **20.4.0**, Atmosphère 1.7, modchip |
| IP | `192.168.1.111` |
| FTP | port **5000**, served by **sys-ftpd-10k** (not DBI) |
| hbmenu netloader | port 28280, armed with **Y** (single connection; probing consumes it) |
| App log channel | app dials `192.168.1.55:28281` (the Mac) |
| Mac IP | `192.168.1.55` |

Sysmodules present: `sys-patch` (sigpatches, boot2), `sys-ftpd-10k`, `Tesla`.
Removed because they crash or conflict: `sys-ftpd`, `sys-ftpd-freeze`, `sys-ftp-breeze`,
`sys-hidplus` (Windows-era build, crashes the boot on 20.4.0).
`mcp-server` (`010000000000B1C0`) exists but never listens on the LAN; ignore it.

Helpers in the repo: `switch/nxlink.py` (push NRO + log), `switch/ftp_game.py` (mirror disc over FTP),
`switch/log_channel.cpp` (the in-app logger).

---

## 2. SD card layout

```
sdmc:/switch/nsmbw/
    Config.toml        dvd_root = "sdmc:/switch/nsmbw"
    sys/  files/  disc/  cert.bin ticket.bin tmd.bin h3.bin   (338 MB extracted disc)
    nsmbw-r<N>.nsp     the installable package (only the newest build is kept)
    boot.log           written by the app at startup
```

`runtime/include/runtime_config.h` returns `sdmc:/switch/nsmbw` as the application data directory
under `__SWITCH__`. `GameFolder::IsExtractedDisc()` needs `files/` plus `sys/fst.bin`.

---

## 3. Toolchain and dependencies (all under `/tmp`, rebuildable)

* Docker image `nsmbw-switch` — devkitA64 + libnx + portlibs (`switch/Dockerfile`).
* `/tmp/deps` — imgui, fmt, abseil, xxhash, tracy, sqlite, libpng, zlib, mbedtls, SDL3 headers,
  and **dawn-src** with `build-switch/` (Dawn + Tint + SPIRV-Tools, OpenGL ES backend).
* Dawn must be configured with `-DCMAKE_C_FLAGS/-DCMAKE_CXX_FLAGS="… -fPIC"` (Dawn's native objects
  are otherwise compiled non-PIC, which puts vtables in read-only memory — see §5).
* Binutils live in `/opt/devkitpro/devkitA64/aarch64-none-elf/bin/` **without** a target prefix
  (`ar`, `objcopy`, `readelf`, `nm`); the compiler is `/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++`.

---

## 4. Packaging an NSP (the part that took longest)

### Title ID
Must look like a **base application**: `0100` + two non-zero groups + ending `0000`.
Example that works: `0100b00b51230000` (the GTA port). Ours: **`01004E53A1B50000`**.

* `0100000000004e53` → DBI says *"titleID from update"* (bit 11 set) and `Creating game… Failed 0x2e4`.
* `…3000` → classified as addon content, same failure.
* With the right shape the install is clean and the title launches.

### NPDM (`npdm.json` → `npdmtool` → `main.npdm`)
```json
{
  "name": "nsmbw", "title_id": "0x01004E53A1B50000",
  "main_thread_stack_size": "0x800000",
  "is_64_bit": true, "address_space_type": 1, "is_retail": true,
  "pool_partition": 1, "process_category": 1,
  "service_access": ["fsp-srv", "set:sys", "time:u", "spl:", "audren:u", "nifm:u"],
  "debug_flags": { "allow_debug": true },
  "kernel_capabilities": [ … from hacBrewPack template … ]
}
```
**npdmtool silently drops service names shorter than 4 characters** (`hid`, `sm:`), and the `"*"`
wildcard produces an empty table. libnx's default `__appInit` then aborts at `smInitialize`/`hidInitialize`.
The port therefore **overrides the weak `__appInit`** (`switch/switch_appinit.cpp`) and initialises only
what is available, ignoring failures. **Do not call stdio from `__appInit` or `__nx_dynamic`** — they run
inside crt0 before libc streams exist, and writing to `stderr` there aborts the process.

`npdmtool` output starts with `META`, not `NPDM`; that is normal for current devkitPro.

### NCA/NSP creation
`hacBrewPack` (the `rlaphoenix` mirror; The-4n's repos are gone), built in the container:

```
hacbrewpack --keyset keys.dat --exefsdir exefs --controldir control --romfsdir romfs \
  --nologo --plaintext --titleid 01004E53A1B50000 --titlename "NSMBW rN" --tempdir /tmp/hbp --nspdir out
```
`keys.dat` must be a **trimmed** keyset: `header_key` plus `key_area_key_application_00..0F` only.
The Ryujinx `prod.keys` contains a malformed 17-byte `master_kek_source_06` that makes hacBrewPack abort.

---

## 5. Link-time invariants (all five were real crashes)

1. **`elf2nso` requires exactly 3 loadable `PT_LOAD` segments.** Do not merge `.rodata` into `.data`;
   the converter emits a corrupt NSO (“Invalid ELF: expected 3 loadable phdrs”).
2. **`.dynamic` must end with `DT_NULL`.** libnx's specs pass `--spare-dynamic-tags=0` and ld emits no
   terminator, so `__nx_dynamic` walks past the section into garbage and faults inside
   `_dynProcessRela`/`_dynProcessRelr`. Fixed by `patch_dynamic.py`, which rewrites the last entry
   (`DT_RELACOUNT`) as `DT_NULL`.
3. **No relocation may target `.rodata`.** The NSO loader maps the middle section read-only, so a
   `R_AARCH64_RELATIVE` store there is a permission fault that Atmosphère reports as *Fault Address 0*.
   devkitPro's prebuilt `libstdc++`, `libc`, `libm`, `libgcc`, `libsysbase` are **non-PIC** and put
   vtables/locale tables there. Fixed by renaming every `.rodata*` section in those archives to
   `.data.rel.ro*` with `objcopy --rename-section …=…,alloc,load,data` and linking the resulting
   `*_pic.a` archives **after** the system libraries. Harbour `libc_pic.a` alone collides on `fcntl`
   with libnx, hence the ordering.
4. **RELA works, RELR is fragile** for this image. Link with `-Wl,-z,nopack-relative-relocs`; the table
   is then 38,921 explicit `R_AARCH64_RELATIVE` entries.
5. **Section-name parsing in scripts**: `readelf -SW` right-aligns the section index, so the name is
   `$2` for some objects and `$3` for others. Extract names with
   `grep -oE '\.rodata[^ ]*'`, never by field position.

Verification that matters (prints in the build):
```
RODATA RELOCATIONS: 0 of 38921      ← must be 0
readelf -d: last entry 0x0 (NULL)   ← must be DT_NULL
```

---

## 5b. The two root causes found on 2026-11-19 (both were in this port, not the game)

### (a) TLS is never bootstrapped: every `thread_local` dereferences null

An NSO carries **no `PT_TLS`**, and **nothing in libnx ever writes `tpidr_el0`** — every libnx routine
reads `tpidrro_el0`, which is the kernel-provided IPC command buffer, not C thread-local storage. So
the register stays 0 and the first access to any `thread_local` reads `0 + tls_offset`.

toml11 caches its parser `spec` in a `thread_local`, so `toml::detail::syntax::ws` faulted at
`0xCA0`. Atmosphère reported every one of these as **`Fault Address: 0`**, which is why it looked
like a null dereference for so long. Ryujinx names it directly:

```
InvalidAccessHandler: Invalid memory access at virtual address 0x0000000000000CA0
```

`switch.ld` reserves `.main.tls` (exactly `.tdata` + `.tbss`) and exports `__tls_start`/`__tls_end`
and `__tdata_lma`, but the CRT step that copies `.tdata` there and points the register at it is
missing. Worse, the linker computes TLS offsets against a base that is **not** `__tdata_lma` (with no
`PT_TLS` the base is arbitrary — it measured `__tdata_lma - 0x10`), so pointing at `.tdata` blindly
still reads the wrong bytes.

`switch/switch_tls.h` solves it by self-calibration rather than a hard-coded delta: it sets
`tpidr_el0` to `__tdata_lma - delta` and walks `delta` back in 8-byte steps until a sentinel
`thread_local` reads back its initial value. That uses the compiler's own addressing, so it stays
correct whatever the linker does. **The sentinel read must live in a `noinline` function** — otherwise
the compiler hoists the `mrs`/`add` out of the loop, computes `tpidr + 0x10` while `tpidr` is still 0,
and faults at `0x10` before calibration starts.

Called first thing from `__appInit`. Verified in a 300 KB probe: `result=1 delta=0x10 read=0x4d2
thread_local ok`.

### (b) Re-PIC-ing *all* of `.rodata` in devkitPro's libraries breaks newlib

The read-only-relocation fix (§5.3) originally renamed **every** `.rodata*` section in libstdc++,
libstdc++fs, libc, libm, libgcc and libsysbase to `.data.rel.ro*`. That silently destroyed newlib's
locale tables. The symptom was subtle and misleading — literals still printed, but every conversion
vanished:

```
re-PIC (all):   B int=      C str=[      D char=     E setlocale null   F newlocale null
stock libs:     B int=42    C str=[abc]  D char=z    E setlocale ok     F newlocale ok
```

`setlocale`/`newlocale` returning NULL and `std::locale::classic()` killing the process follow from
the same damage, and it is what made `<iostream>`'s global initialiser fatal.

The rewrite must be **surgical**: rename only a section that a relocation actually targets, i.e. the
`<section>` named by each `.rela.<section>` entry. On these libraries that is 821 sections in
libstdc++, 24 in libstdc++fs, **9 in libc**, 1 in libsysbase, 0 in libm/libgcc — versus hundreds in
libc for the blanket rewrite. With the targeted version the probes match stock exactly *and* the
link still reports `RODATA RELOCATIONS: 0 of 38921`.

## 5c. Better than re-PIC-ing: put `.rodata` in the writable segment

The library surgery in §5.3 is no longer needed. `switch.ld` assigns output sections to explicit
program headers:

```
PHDRS
{
    code   PT_LOAD FLAGS(5);   /* RX */
    rodata PT_LOAD FLAGS(4);   /* R  */
    data   PT_LOAD FLAGS(6);   /* RW */
}
```

and `.rodata` is emitted `:rodata`. If it is emitted `:data` instead, every one of the 1,810
relocations that used to land in a read-only page becomes a normal writable store, and devkitPro's
prebuilt libraries link **unmodified** — which also removes the newlib/locale breakage in §5b(b)
entirely.

`switch/make-rw-ldscript.py` generates `switch-rw.ld` and a matching `switch-rw.specs`. Two details:

* The `.rodata` block must be **moved to the end of the read-only group**, not retargeted in place:
  `.tls.align`, `.eh_frame`, `.gnu.version*` and `.note.gnu.build-id` follow it and stay `:rodata`,
  and a `:rodata` section placed after a `:data` one produces overlapping LMAs.
* The `-T` has to come from the **specs file**. Passing a second `-T` on the command line leaves both
  scripts in effect, which resets the location counter and fails with
  `section .tls.align LMA [0,7] overlaps section .text LMA`.

The read-only segment keeps the dynamic tables and the unwind tables, so elf2nso still sees three
loadable segments. Verified with stock libraries: `.rodata` lands at `0xef000`, exactly the start of
the RW segment, and a 300 KB probe runs `RuntimeConfigFile::Get()` end to end
(`CFG opened → CFG parsed → CFG loaded → done`).

Build with:

```
SPECS=/src/switch/switch-rw.specs \
RUNTIME_LIBS="-L/opt/devkitpro/devkitA64/aarch64-none-elf/lib -lstdc++ -lc -lm -lgcc -lsysbase" \
./switch/rebuild.sh --pack
```

## 6. Current build pipeline

Order matters; scripts are in `switch/`:

1. `build-pie.sh` — every object (runtime, Aurora, generated, game shards, libs, absl, mbedtls, shims)
   compiled `-fPIE` into `/src/obj-pie`. Parallelise with `xargs -P $(nproc)`; sequential loops use one
   core and make `docker stats` look like 99% while the Mac idles.
2. `make-piclibs.sh` — derive `libstdc++_pic.a`, `libstdc++fs_pic.a`, `libc_pic.a`, `libm_pic.a`,
   `libgcc_pic.a`, `libsysbase_pic.a`.
3. `link-game.sh` — link with stock `switch.specs`, `-Wl,-z,nopack-relative-relocs -Wl,-z,notext`,
   PIC archives after `-lnx -lEGL -lGLESv2 -lglapi`, then `patch_dynamic.py`, then `elf2nso`.
4. `pack-nsp.sh` — `npdmtool` + `hacbrewpack`, upload to the console as `nsmbw-r<N>.nsp`, delete older.

Platform layer (`switch/`): `sdl_switch.cpp` (SDL3 subset on libnx: window, pad, clock, events, paths,
IO), `switch_shims.cpp` + `include/switch_posix.h` (Horizon gaps: anonymous mappings for Abseil,
`strerror_r`, `poll`, `pipe`, `sigfillset`, UTC time globals), `absl_horizon.cpp`, `log_channel.cpp`,
`discord_stub.cpp`, `hd_texture_stub.cpp`, `switch_appinit.cpp`.

Renderer: Dawn `libwebgpu_dawn.a` built for `aarch64-none-elf` with the OpenGL ES backend; Aurora's
`BackendBinding.cpp` passes libnx's `NWindow*` through Dawn's opaque native-window descriptor, and
`Surface.cpp`/`SwapChainEGL.cpp` have `__SWITCH__` cases. `SystemEvent.cpp` degrades to polling because
Horizon has no `pipe(2)`.

---

## 7. Where it currently stops

Static initialisation now runs **77 of 122 constructors** (it was 1 before TLS and the library fix).
It stops inside `_GLOBAL__sub_I__ZN16settings_overlay25InitializeRuntimeSettingsEv`, the initialiser
for `settings_overlay.cpp`, which reads `RuntimeConfigFile::Get()` about 25 times.

Diagnostics in place for the next step:

* `sdmc:/switch/nsmbw/init.log` — one line per `.init_array` slot, written by a trampoline that
  `log_channel.cpp` installs over the array from `.preinit_array`; the last index is the initialiser
  that never returned. Slot 77 is the current stop.
* `sdmc:/switch/nsmbw/boot.log` — the preinit line, now including `tls=<result> delta=<n>`.
* `switch/probe*.cpp` + `switch/build-probe.sh` — 300 KB probe NSPs that reproduce runtime-level
  problems without the 64 MB package. `build-probe.sh <name> <src> [defines]`, `STOCK=1` to link
  devkitPro's untouched libraries for an A/B test. This is how both root causes were found.
* `switch/rebuild.sh [units...] --pack` — incremental rebuild: recompile named units, relink, verify,
  package. Roughly two minutes.

## 8. Known remaining work (after the boot fix)

1. Reach `RuntimeMain`: config load, `GameFolder`, guest arena.
2. Aurora + Dawn device creation on EGL/NWindow (GLES is proven on the console via Mesa).
3. Audio (`audren`), input (`hid` — blocked by the npdmtool 3-char drop, needs a different NPDM writer).
4. Frame dump for visual evidence (`sdmc:/switch/nsmbw/frame_*.png`), planned but not implemented.
5. NSP polish: icon, version string, and eventually replacing the fake-ticket package.

## 9. Artifacts

* Repo: `~/Projects/nsmbw-native`, branch **switch-port** (commit `ab90d28` + later).
* Working tree: `/tmp/nsmbw-switch` (obj-pie, piclibs, wiicompiled.elf, scripts) — **/tmp is cleared on
  reboot**, so rebuild from this document and the repo if it disappears.
* NSP staging: `/tmp/nsmbw-pack` (`keys.dat`, `npdm.json`, `exefs/`, `control_g/`, `out_rN/`).
* Keys: `~/Documents/Ryujinx/Ryujinx/ProdKeys.NET-v22.0.0.zip` (`prod.keys`; extract, then trim).
