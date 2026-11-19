# Patched runtime sources for the Switch port

`switch/port/` mirrors the paths these files came from in the working tree (`/tmp/nsmbw-switch`),
which does not survive a reboot. Copy them back over a fresh checkout:

```
cp -R switch/port/runtime/src/*     /tmp/nsmbw-switch/runtime/src/
cp -R switch/port/runtime/include/* /tmp/nsmbw-switch/runtime/include/
```

Everything under `switch/` itself is already the current version.

## What is changed and why

### `runtime/src/guest_flat_memory.cpp` — Horizon branch in `GuestFlat::Initialize`

Real port bug, not instrumentation. `CreateSharedMemory()` returns -1 under `__SWITCH__` ("no
shared-memory objects on Horizon; the checked arena path does not need them"), but `Initialize()`
still took the shared section/`memfd_create`/`mmap` path and threw at that -1. Horizon now records
one `MappedRegion` per request straight from the arena that `EnsureReservation()` already allocated
(`g_base + region.base` is the host view) and returns before the section machinery.

### `runtime/src/main.cpp` — `argc`

libnx reports `argc == 0` for a title launched from the home menu, which tripped the
"the game runtime does not accept command-line options" `invalid_argument`. Under `__SWITCH__` the
check is skipped: Horizon hands over launch parameters from the title metadata, not command-line
options, and every setting comes from `Config.toml` anyway.

### Temporary trace instrumentation

`switch_trace_mark()` calls in `RuntimeMain` (`main.cpp`), `SystemBridge::Initialize`
(`system_bridge.cpp`), `Memory::Init` (`memory.cpp`), `GuestFlat::Initialize`
(`guest_flat_memory.cpp`) and `RuntimeConfigFile::Get`/`LoadConfigFile` (`runtime_config.h`), plus the
`log_channel.cpp` init-array trampoline. These write to `sdmc:/switch/nsmbw/init.log` one line per
step and are what located every blocker in §5b/§5c of the handoff doc. Remove them once the port is
stable; they cost a file open/close per line.

### `runtime/include/runtime_config.h`

Marked only, plus `LoadConfigFile` split so the `ifstream` construction and the parse are separable.
No behaviour change.

## Abseil

See `switch/patches/absl-switch.md` — those edits live in `/tmp/deps/absl`, not here.

## Open bug (2026-11-19): `.bss` corruption during static initialisation

`GuestFlat::Initialize` is fixed, `SystemBridge::Initialize` now gets through `Memory::Init`, low
memory seeding, `InitializeDataSections` and `InitializePersistentCpuContext`, and reaches the DOL
constructor loop. It then dies looking up the first constructor (`0x802DD2E0`) in
`TranslatedFunctionRegistry`.

Measured with probes at `.bss` (see `BSS@appInit` / `BSS@SB` in `init.log`):

```
BSS@appInit: 00000000 00000000 00000000 00000000     .bss is zeroed by the loader
BSS@SB:      00000000 00000001 00000001 00000000     +0x40 and +0xC0 became 1
```

`Registry()::entries` lives at `__bss_start__ + 0xC0` (confirmed with `nm`; its guard variable is at
`+0x140`, so this is not a guard mix-up), so `_M_start` is literally `1`:

```
REG: vec=0xe8b70c0 data=0x1 size=5270498306927591953 cap=0
```

RESOLVED — see the TLS aliasing section below. The init-array trampoline was ruled out by disabling it.

Next step: re-enable the trampoline (`install_init_trace()` in `log_channel.cpp`) and have it record
the word at `__bss_start__ + 0xC0` after every constructor, to name the one that writes it.

## RESOLVED (2026-11-19): `.bss` corruption was TLS aliasing the globals

The word watch named the writer: constructor 77 (`settings_overlay`), value `1`.

```
16=0x740 ... 49=0x241d80   ← the registry vector growing with real heap pointers
77=0x1                     ← storage pointer replaced by 1
```

`switch.ld` places `.tbss` and `.bss` at the **same virtual address** (`0x63b7000`), because `.tbss`
is NOBITS and TLS addresses are offsets, not real addresses. The first TLS bootstrap pointed
`tpidr_el0` at `.tdata`, so every `thread_local` living in `.tbss` resolved into `.bss` and wrote over
a real global. `TranslatedFunctionRegistry`'s vector storage was one of the victims.

Fix: bootstrap against the reserved `.main.tls` region (`__tls_start`..`__tls_end`), which exists for
exactly this and aliases nothing. `switch_tls.h` seeds it from the `.tdata` image, zeroes the `.tbss`
tail, then calibrates the base offset against the sentinel as before.

Result: the registry is healthy (`data=0x200241d80 size=396 cap=512`), all **184** DOL constructors
run, `SystemBridge` and `TranslatedFunctionRegistry::Finalize` complete, and **`aurora_initialize()`
succeeds**. The next fault is a guest-derived address (`0x1802E7000`, i.e. the arena base plus
`0x2E7000` for guest `0x802E7000`).

## Current state after the TLS fix

The process no longer crashes. `init.log` ends with:

```
RM: aurora initialized
REG: frozen lookup 00000000
REG: frozen find done
```

and Ryujinx stays on its "Loading" splash with the guest alive. A sample of the live process shows
every thread parked in `__psynch_cvwait` — the guest is **blocked on a condition variable**, not
spinning.

Two things pinned before that point:

* Guest address 0 was being dereferenced (`GF: OUT OF ARENA guest=00000000 host=0x1802e7000`, where
  `g_base == 0x1802e7000` exactly) and a null callback was dispatched (`REG: frozen lookup 00000000`).
  A 64 KiB zero page now backs any guest access below `0x80000000`, because Horizon cannot reserve
  the 2 GiB the arena is offset by. That removed the fault without hiding it: every hit is logged as
  `GF: NULLPAGE guest=...`.
* `RM: frame worker callback` — the statement immediately after `aurora_set_frame_worker_wait_callback`
  — is never reached, so the main thread blocks there or in the statements just before it.

Next: decide whether `aurora_set_frame_worker_wait_callback` is deadlocking against the frame worker
that `aurora_initialize` starts (the worker resolves a null callback before the main thread installs
the real one), or whether the worker is waiting for a frame the main thread never submits. If it is
the former, install the callback before `aurora_initialize` or gate the worker on a flag it can see.

## Registration shards must be compiled

`build-pie.sh` only globs `generated/build_shards/base_common/`, so the translator's 16 bulk
registration shards in `generated/build_shards/base_registration/` were never compiled. The registry
therefore held 396 entries instead of the full function map, and `ResolveEntry()` failed with:

```
No translated function registered at address 0x80004050
```

`0x80004050` *is* in `registration_08_f085cbf1d5c0252e.cpp` (`{0x80004050u, "func_80004050", ...}`).
`switch/build-registration.sh` compiles all 16 into `obj-pie/gen/` (which every link globs), adding
about 79,000 relocations — all of them in the writable segment, so still fine.

With that, the entry resolves and translated guest code runs for the first time:

```
SB: dol ctor 0 @802ED9E0
SB: read 802DD2E0
SB: translated found
SB: invoke 802DD2E0      ← first translated Wii function invoked
REG: lookup 802dd2e0     ← and it calls back into the registry
```

Startup now stops inside the game's first DOL constructor, with no crash artifact and no fault
report.
