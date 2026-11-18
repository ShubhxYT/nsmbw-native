# Questions this project answers

Short, direct answers. Each one is a question people actually type into a search box.

## Can I play New Super Mario Bros. Wii on a Mac without an emulator?

Yes. This repository builds a native macOS app from your own disc. The game's PowerPC code is
translated ahead of time to C++ and compiled for Apple silicon, so the game runs directly on the
chip — no emulator, no JIT. See the [README](../README.md) for the one-command build.

## Is there a native New Super Mario Bros. Wii port for Windows?

Yes. `make-nsmbw.ps1` builds a `.exe` folder that runs on Windows 10/11 x64 with D3D12 or Vulkan,
plus a desktop shortcut. Same source, same disc.

## How do I play New Super Mario Bros. Wii on an iPad or iPhone?

Build an unsigned IPA with `make-nsmbw-ios.sh` and install it with **LiveContainer** (or SideStore).
The app carries the game data inside the bundle. Full steps, including the iOS patch and the Xcode
requirements, are in [ios.md](ios.md).

## Does this include the game, the ROM, or the keys?

No. There is no game code, no assets, no keys and no disc image anywhere in this repository — only
scripts and patch code. You need your own dump of the Wii disc (USA `SMNE01`, revision 1).

## Which version of the game is supported?

The USA release, disc ID `SMNE01`, revision 1, as `.rvz`, `.iso`, `.wbfs`, `.gcz` or `.wia`. A dump
whose `main.dol` checksum differs from the table is still accepted if the rest of the disc identity
(FST size, entry point, REL prelink) validates it; that usually means a patched executable.

## Is this an emulator?

No. An emulator interprets or JIT-compiles the game while it runs. Here the game is translated once,
ahead of time, into C++ and compiled to a normal program. That is why it starts instantly, holds
60 fps on modest hardware, and why an IPA can run inside LiveContainer without any JIT entitlement.

## How do the graphics work?

The Wii's GX graphics API is reimplemented on top of Dawn: **Metal** on macOS and iOS, **D3D12 or
Vulkan** on Windows. Shaders are translated ahead of time rather than at runtime.

## Can I play online with a friend?

There is LAN play, marked **beta** and desktop only. Rooms are found by UDP broadcast on port 52130,
and Tailscale works for remote players with no port forwarding. Tailscale Funnel and ngrok cannot
carry it — they do not forward UDP. Expect rooms that fail to start or drop.

## Why is controller tilt on L1/R1 on the iPad?

Because iPadOS does not deliver a DualShock 4 or DualSense gyro to apps. The same defect is open in
Moonlight's iOS app. Holding L1 or R1 rolls the emulated remote, which covers the tilting platforms.
Details and sources in [ios.md](ios.md#known-gaps).

## Where are the saves?

| Platform | Save file |
|---|---|
| macOS | `~/Library/Application Support/WiiCompiled/NAND/title/00010000/534d4e45/data/wiimj2d.sav` |
| Windows | `%USERPROFILE%\Documents\MarioWiiSaveData\NAND\title\00010000\534d4e45\data\wiimj2d.sav` |
| iPad / iPhone | `Documents/WiiCompiled/NAND/title/00010000/534d4e45/data/wiimj2d.sav` |

On iOS that folder lives inside the app's container; LiveContainer deletes the old container when an
app is reinstalled, so keep a copy outside it.

## What is known to be broken?

Two things, both documented: a crash when returning to the world map after a course can end, and the
missing controller gyro on iOS. See [known-issues.md](known-issues.md).

## Does it run on an Intel Mac?

The macOS build targets Apple silicon. The runtime and the translator are portable, but Intel Macs
are untested here, and the Windows build is the supported non-Apple path.

## How long does a build take?

Roughly 20–30 minutes the first time (translating ~39,000 functions and compiling the game plus
Dawn and SDL3), then a couple of minutes for a rebuild, because translation, CMake configure and
unchanged compiles are reused.
