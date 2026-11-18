# Troubleshooting

Everything here was met while building this, on a Mac and on a Windows 11 machine.

## The game hangs at boot, or a black window with 0 frames

Two different causes, both seen:

**"0 guest frame(s) presented, VI black=1" in the log.** On Windows, launching the exe from an SSH
session (or any non-interactive session) fails to create a Direct3D swap chain:

```
[aurora] WebGPU device was lost: Creating the IDXGISwapChain1 failed with
         DXGI_ERROR_NOT_CURRENTLY_AVAILABLE (0x887A0022)
```

That is the launch context, not the game. Start it from the desktop shortcut inside your logged-on
session and it runs normally.

**A silent spin after the engine initialises.** If the runtime log stops after `NW4R - EF` and the
process sits at ~100% of one core, the BCA patch is missing. Confirm with
`python3 apply-patches.py <repo>` — it should report "BCA check answered natively".

## `sys/main.dol differs from the clean ... executable`

The scripts handle this: they record your dump's checksum and continue, after checking the disc ID,
revision, FST size and entry point. A difference usually means a patched executable. If the game later
misbehaves at a consistent spot, that mismatch is suspect #1 — try a clean dump of the same version.

## `No .NET SDKs were found` / `NU1100: Unable to resolve ...`

The translator is a .NET 8 program.

```powershell
winget install Microsoft.DotNet.SDK.8
```

`NU1100` on every package means the machine has **no NuGet sources** even though nuget.org is
reachable. Check and repair:

```powershell
dotnet nuget list source
dotnet nuget add source https://api.nuget.org/v3/index.json -n nuget.org
```

Both scripts now do this automatically when the list is empty.

## Windows: `No pinned sources in ...\Dependencies`

`Prepare-Dependencies.ps1` resolves a relative `-Destination` against its own folder, so the packages
land next to the script instead of in the checkout. The Windows script passes an absolute path now. If
you hit it with an older copy, move the folder:

```powershell
Move-Item <scripts>\Dependencies <repo>\Dependencies
```

## macOS: `metal: command not found` / shaders fail to compile

The Metal toolchain is a separate download in recent Xcode:

```bash
xcodebuild -downloadComponent MetalToolchain
```

## Build is very slow

The first build is: translate ~39k functions, compile ~200 MB of generated C++, plus Dawn and SDL3.
Roughly 20–30 minutes on an M4 Pro or an i9-13900H, and the toolchain download on Windows adds to
that. Reruns reuse everything unchanged — a second build is a couple of minutes.

If it looks stuck, check the stage: `logs/` in the work folder has one log per stage
(`configure.log`, `build.log`, `translate.log`, ...). On Windows also check
`projects\nsmbw\tools\rebuild-*.log` inside the checkout.

## Remote play hitches, then skips ahead

That is the input buffer, and it is the whole difference between smooth and stuttery in a lockstep
session. Every device must hold a frame until all players' inputs for it have arrived. If the buffer
is smaller than the time the slowest input needs, the frame waits — everyone freezes — and then the
frames already sitting there run back to back, which looks like the game skipping to catch up.

- **F10 › LAN › Input delay** is the knob. `auto` sizes it from the room's ping and jitter at the
  moment the room starts; it is read when a room starts, not live.
- The top of the screen shows `N ms  delay N` for the whole session: the round trip (a client to the
  host, the host each client) and the delay in force. Watch those two while tuning rather than the
  game's feel alone.
- Raise it if it hitches and skips. Lower it if every button feels late and it never hitches.
- A two player session needs roughly half the ping; three or four players need roughly the whole
  ping, because the host relays each client's input to the others.
- If it needs more than about 8 frames (135 ms) to stay smooth, the route is the problem: check
  `tailscale ping <other machine>`. A line that says `via DERP` is going through a relay hundreds of
  kilometres away; a direct connection (`direct`) is what you want. Opening UDP 41641 on the host's
  router or turning off a VPN often flips it back to direct.

## Tunnels: Funnel and ngrok will not carry this

The session is **UDP** on port 52130 (plus a discovery broadcast). Two popular tunnels cannot carry
that:

- **Tailscale Funnel** only forwards TCP/HTTPS (ports 443, 8443, 10000) and is bandwidth limited. It
  cannot carry this session at all.
- **ngrok** has no UDP endpoints on any plan.

What does work: plain **Tailscale** between the players (its own relay, DERP, is UDP-capable), a
**playit.gg** UDP tunnel, **frp** on a VPS, or the host having a public address or port forward.
Sending this traffic through TCP would add head-of-line blocking on top of the very latency the
buffer exists to absorb.

## LAN play: rooms are not found, or fail to start

LAN play is **beta**.

- Both players must be on the same build and the same game version.
- Discovery is UDP broadcast on **52130**; a firewall blocking it stops rooms from appearing. For
  remote play, use Tailscale — the lobby asks `tailscale status` for peers by itself, and you can add a
  device by hand under F10 › LAN.
- A room that lists but will not start usually means one side failed the session restart. On macOS
  that was an upstream gap; the patch in this repo fixes it. Run
  `apply-patches.py <repo>` and check it reports "macOS netplay restart added".
- Only the host keeps progress: a session plays on the host's save, and it is written back when the
  host leaves (or recovered at next launch if it was killed).

## Controllers

- A pad is picked up automatically when connected; mappings persist per device.
- If the wrong pad is player 1, unplug/disable the other one, or press a button on the one you want
  to use first.
- Wrong button names? F10 › Controls › *Show names as* forces PlayStation, Xbox or Nintendo labelling.

## Saves and logs

| | |
|---|---|
| macOS saves | `~/Library/Application Support/WiiCompiled/NAND` |
| macOS logs | `~/Library/Application Support/WiiCompiled/Logs/<run>/console.log` |
| Windows saves | `%USERPROFILE%\Documents\MarioWiiSaveData\NAND` |
| Windows logs | `%USERPROFILE%\Documents\MarioWiiSaveData\Logs\<run>\console.log` |

Back up the save before testing new builds. `wiimj2d.sav` is the file that matters.

## iOS

The iOS port hit its own set of failures. All of them are fixed by `patches/ios-port.patch`; they are
listed here because the messages are cryptic and each one means something specific.

| Message | Cause | Fix in the patch |
|---|---|---|
| `memfd_create for guest RAM failed (Operation not permitted)` | iOS has no `memfd_create`, and the POSIX shared-memory fallback is denied inside the app sandbox | guest RAM is an unlinked file in the app's `TMPDIR` |
| `'IOSurface/IOSurface.h' file not found` | that umbrella header is macOS only | iOS uses `IOSurfaceRef.h` |
| `mach_vm.h unsupported` | iOS forbids the Mach VM API | the portable `mmap`/`mprotect` path is used instead |
| `'CoreAudio/CoreAudio.h' file not found` | iOS has no Core Audio process-object graph | the external-audio query is stubbed |
| app exits with no window | `SDL_MAIN_HANDLED` means nothing sets up `UIApplicationMain` | `main` calls `SDL_RunApp` |
| `@rpath/libpng16.dylib` not found on device | a dependency was built shared | dependencies are static for iOS |
| black screen, no game window | the runtime died before SDL started | read `Documents/WiiCompiled/Logs/<run>/console.log`; the last line names it |

The log's first line is `[runtime] WiiCompiled <version>`, from `build-fingerprint.json` in the
bundle, so a pasted log always says which build produced it.
