# Dawn edits required by the Switch port

`/tmp/deps/dawn-src` is a scratch tree; these three edits must be reapplied whenever Dawn is rebuilt.
Each was found by reading Aurora's `[aurora::gpu]` log lines in `boot.log`.

Apply them with `switch/rebuild-dawn-object.py`, which recompiles one translation unit using Dawn's
own `compile_commands.json` and replaces the member in `libwebgpu_dawn.a` (~1 minute instead of a
full 15-minute Dawn rebuild). It runs `ar s` afterwards; without the rebuilt symbol index the linker
keeps using the old object, which cost a debugging cycle.

## 1. `src/dawn/native/opengl/DisplayEGL.cpp` — no `dlopen`

Horizon has no dynamic loader and the port links libEGL statically, so `DiscoverAdapters` could never
load `libEGL.so`:

```
Dawn: Failed to load libEGL.so: "dlopen is not available on Horizon".
```

Use Dawn's existing static path instead — the linked `eglGetProcAddress` plus
`InitializeWithProcAndDisplay`, which creates the default display itself:

```cpp
#if defined(__SWITCH__)
    (void)libName;
    (void)searchPaths;
    return InitializeWithProcAndDisplay(reinterpret_cast<EGLGetProcProc>(&eglGetProcAddress),
                                        EGL_NO_DISPLAY);
#else
    ... original body ...
#endif
```

## 2. `src/dawn/native/opengl/BackendGL.cpp` — optional adapter extensions

Mesa's Switch EGL advertises neither `EGL_EXT_create_context_robustness` nor
`EGL_KHR_fence_sync`/`EGL_KHR_reusable_sync`, and `AppendNewDeviceFrom` treats both as fatal. Under
`__SWITCH__` both checks are compiled out (the surrounding `DAWN_UNRECOVERABLE_ERROR`s are replaced
by a comment). Without this, adapter discovery never succeeds.

## 3. `src/dawn/native/opengl/PhysicalDeviceGL.cpp` — no robustness context attributes

`DeviceBase::Create` passed `useRobustness` from a toggle, and the robustness attributes made
`eglCreateContext` fail with `EGL_BAD_ATTRIBUTE` because the extension is absent:

```cpp
#if defined(__SWITCH__)
    bool useRobustness = false;
#else
    bool useRobustness = !deviceToggles.IsEnabled(Toggle::DisableRobustness);
#endif
```

## 4. `src/dawn/native/opengl/QueueGL.cpp` — no fence-sync extension

`Queue::Queue` picks a fence type from the EGL extensions and ends in `DAWN_UNREACHABLE()` when none
is advertised. Relaxing patch 2 leaves exactly that path reachable, and `DAWN_UNREACHABLE()` is
undefined behaviour: it **hung device creation silently** rather than reporting anything, which cost
several cycles to find. Under `__SWITCH__` fall back to `EGL_SYNC_FENCE`.

This is the patch that lets `aurora_initialize()` return.

## Link line

Once Dawn references EGL directly, the linker pulls Mesa's `egl_switch.c`, which needs the nouveau
winsys and `fatalThrow` from libnx. Append both, because `-lnx` is scanned before `-lEGL` and cannot
satisfy it there:

```
RUNTIME_LIBS="... -lsysbase -ldrm_nouveau -lnx"
```

## Where it stands

With all of the above:

```
[aurora::gpu] Graphics adapter information
  API: OpenGLES
  Device: NV120 (Unknown)
  Driver: OpenGL version OpenGL ES 3.2 Mesa 20.1.0-rc3 on NV120
[aurora::gpu] Using limits: maxTextureDimension2D: 16384 ...
```

The adapter is found and device creation begins, then stalls after printing the device limits, with
no error — `aurora_initialize()` never returns. That is the next target.

## Result

```
DAWN: dev buffers
DAWN: device created
RM: aurora status=0 error=(none)
RM: gx hooks
RM: reverb
RM: frame boundary
RM: runtime settings
RM: updater
RM: loading screen
```

Aurora fully initialises, and startup now stops later, at guest entry resolution:

```
REG: frozen lookup 80004050
No translated function registered at address 0x80004050
```

Diagnostic marks were added to `PhysicalDeviceGL.cpp`, `ContextEGL.cpp`, `DeviceGL.cpp` and
`QueueGL.cpp` (all `switch_trace_mark` calls); they are cheap to leave in and were what located the
stall.

## Diagnostics: catching host exceptions that escape into guest code

`switch/cxa_throw_probe.cpp` is linked with `-Wl,--wrap=__cxa_throw` and
`-Wl,--wrap=_ZSt9terminatev`, so it logs every C++ throw (type, `what()`, and the last translated
dispatch address) and every `std::terminate` without disturbing libstdc++:

```
THROW St12system_error from guest=801b5b00 what=Function not implemented
TERMINATE from guest=801b5b00
```

Two lessons from it:

* The `toml::value::at` `out_of_range` throws (`discord`, `retro_rewind_root`, `overlay_roots`) are
  expected and caught — they are not the fault.
* **`RecompMod::CurrentTranslatedExecutionAddress()` is the last *dispatched* target, not the throw
  site.** It stays pinned to the most recent `Invoke*Cpu`, so it does not identify where a host
  exception originated. Do not treat it as a fault address.

The real fault is `std::system_error` with `ENOSYS` ("Function not implemented") escaping into guest
code, which has no handler, so it becomes `std::terminate`. Adding a `catch(std::exception)` around
`OSSuspendThread`'s body did not change it, which confirms the throw happens elsewhere.
