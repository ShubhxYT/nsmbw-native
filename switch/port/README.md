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
