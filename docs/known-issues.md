# Known issues

Open problems, with everything that is known about them so far. Each one lists what was ruled out,
because that is usually the expensive part to rediscover.

## Crash returning to the world map after a course ends

**Status:** open. Deterministic; not yet reproduced on demand.

**Symptom.** Leaving a course for the world map (scene `0x003`) sometimes stops the game with:

```
[runtime] Unhandled C++ exception: Memory access violation at 0x55440011 (+0x4) :: no mapped region
[runtime] fatal dialog: The game stopped because an unhandled C++ exception occurred.
```

Captured twice, at the same guest instruction both times:

| | |
|---|---|
| Guest PC | `0x800C92F0` — `AnimeStartSetup__9LytBase_cFib` |
| Called from | `func_8077B4B0`, through the object's vtable (`[this + 0xC]`) |
| Fault | 4-byte read at `0x55440011`, no mapped region — a pointer that is not a guest address |
| Last log line | `autosave: course left (exit mode 0)` and `(exit mode 1)` — i.e. the course goal and a lost course |
| Scene | `WORLD_MAP`, immediately after the transition |

The map is starting a layout animation on an object whose resource pointer is stale. This is guest
state, not host state: the game's own world-map state machine is reaching that call with a pointer
it should have been given at load time.

**Not network play.** Both devices logged the identical scene history and reached the same guest
frame (`34093`); the macOS side then logged the fault above and the Windows side's log stops at that
same transition. Two machines in lockstep do not diverge on their own, so the same inputs are enough
to reach this state offline — it should reproduce in single player. (Only the Mac produced a fault
dump; the Windows process was closed with the session.)

**Not fixed yet, and why.** There is no crash dump to diff and no reliable reproduction. A fix means
finding what the world map expects to have been initialised (most likely the animation resource for
a course node) and either restoring it on the path that is taken here, or making the port take the
game's own path instead. That is a change to how scene actors carry their resources — not a
one-line guard. Wrapping it in a check would hide the symptom and leave the map animating nothing.

**Workarounds.** None confirmed. The autosave writes a cleared course to the save as the map loads,
so a crash at that moment can lose the clear — replay the course if the star-coin/door state looks
wrong afterwards. Leaving a course twice in a row in the same session did not crash the first time,
so the trigger depends on map state, not simply on returning.

**What would help:**

- A save file from just before it happens (`wiimj2d.sav`, in the NAND below). With a matching save
  the entry can be scripted instead of played.
- The run's log: `~/Library/Application Support/WiiCompiled/Logs/<run>/console.log` (macOS) or
  `%USERPROFILE%\Documents\MarioWiiSaveData\Logs\<run>\console.log` (Windows).
- Whether it also happens in single player. That single answer says whether the session's NAND
  matters at all.

**Related:** the `Exit Course` option in a course that has never been cleared is a port patch
(`runtime/src/hle/pause_exit.cpp`). It is a candidate to bisect — disabling it is the cheapest way
to check whether the extra exit path is what leaves the map in this state.

## LAN play is beta

Rooms can fail to start or drop. See [troubleshooting.md](troubleshooting.md#lan-play-rooms-are-not-found-or-fail-to-start)
for the buffer, discovery and tunnel notes. This is a known limitation of the mode, not a bug report.
