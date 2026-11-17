# How it works

New Super Mario Bros. Wii is a Wii game: PowerPC CPU, big-endian, fixed-function GX graphics. This
kit does not emulate any of that at runtime. It translates the game ahead of time and compiles the
result, so the finished app runs native code on your CPU.

```
your disc
   │  nodtool extract
   ▼
sys/ + files/            extracted disc (338 MB)
   │  unpack_game.py      copies main.dol, unpacks the four boot RELs
   │  prelink_rels.py     links the RELs the way the game links them at boot
   ▼
game/                    main.dol + one synthetic nsmbw_linked.rel
   │  Translator.Cli     39,114 functions → C++
   ▼
generated/               function sources + build shards
   │  cmake + ninja      compiles shards + the runtime (Dawn, SDL3)
   ▼
WiiCompiled             the game, as a native program
   │  packaging
   ▼
.app (macOS)  /  .exe + game/ (Windows)
```

## The four moving parts

**1. The translator.** Reads the PowerPC executable and writes C++ where each instruction becomes a
plain statement against a CPU context. That is why the build takes tens of minutes: it is compiling a
game, not loading one. `--threads`/`-Jobs` control the parallelism.

**2. The runtime.** Stands in for the Wii's operating system (threads, timers, NAND, disc, audio,
controllers) and implements the GX graphics API on top of Dawn — Metal on macOS, D3D12 or Vulkan on
Windows. Upstream calls it WiiCompiled; the output binary keeps that name.

**3. The patches** (`apply-patches.py`). Four fixes the game needs that upstream does not have:

| Patch | Why |
|---|---|
| BCA check | The game polls the disc's burst-cutting area (a lead-in area no host extraction carries) and spins on a DI interrupt that never comes. Answered natively as "no BCA". |
| macOS netplay restart | Upstream relaunches the process to enter a LAN session, but only has Windows/Android/Linux branches — on macOS it fell through to `_Exit(0)`, killing every session. |
| Button labels | PlayStation / Xbox / Nintendo naming, selected in F10, saved as `[controller] button_layout`. |
| LAN beta label | Rooms can fail to start; the player is told before joining one. |
| Input buffer | Lockstep can only run as fast as the slowest input, so the buffer must cover the real ping and jitter. It was half the round trip plus a fixed 12 ms, rounded down, capped at 8 frames — too small for anything past a LAN, which shows up as a hitch followed by frames skipping to catch up. |

**4. The packaging.** On macOS the app is self-contained: binary, `dsp_coef.bin`, pipeline cache,
bootstrap NAND seeds and the whole extracted disc live inside `Contents/Resources`, and the launcher
writes a `dvd_root` pointing at them if the player has not set one. On Windows the exe sits beside
`game\` and its DLLs, and the script only copies what runs — not the compiler's libraries.

## Why a dump can differ

The sources pin a `main.dol` checksum per game version. A dump whose executable differs is usually a
patched one (region-free or video-mode patches), not a different game. The scripts check the rest of
the identity — disc ID, revision byte, FST size, entry point, and whether the REL prelink predicts
the same load addresses — and if all of that agrees, they record the new checksum and continue, with a
warning that names it as the first suspect for later misbehaviour.

## Network play

Rooms are discovered by UDP broadcast on port 52130 (and by asking devices in `tailscale status`).
A session copies the host's save into a per-session NAND, restarts both programs into it and runs them
in lockstep with an input delay. On exit the host's session save is copied back over its own save; a
host that was killed has it recovered at next launch. Joiners keep no save — the host's world is the
session.

Because it is lockstep, latency is a buffer, not a prediction: every device must hold each frame until
every player's input for it has arrived. The buffer is sized when the room starts from the worst
measured round trip and its spread, plus one extra one-way trip in three and four player sessions
(the host relays a client's input on to the others). Too small and the frame waits, everyone freezes
for a moment, and then the frames that were already waiting run back to back — that burst is the
"skipping to catch up" players notice. It can be set by hand in F10 › LAN.
