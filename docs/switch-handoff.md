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

* `boot.log` proves the preinit constructor runs.
* No new Atmosphère crash report is produced for this build, and no further log lines appear, so the
  failure is in a **static initialiser** (after the priority-101 logger) or very early in `main()`.
* Next instrument: add `fprintf(stderr, …)` markers between the runtime's initialisers, or run the NSP
  under **Ryujinx** on the Mac (`~/Documents/Ryujinx/Ryujinx/Ryujinx.app`, keys and firmware already
  installed, `~/Library/Application Support/Ryujinx/games`) to get a stack trace without the console.

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
