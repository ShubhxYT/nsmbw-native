# Dawn edits required by the Switch port

`/tmp/deps/dawn-src` is a scratch tree; these edits must be reapplied whenever Dawn is rebuilt.
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
several cycles to find. Set `mEGLSyncType = EGL_NONE` under `__SWITCH__` to let the device initialise.

The first submitted frame exposed a second problem: this EGL implementation has no usable
`eglCreateSync` entry point. Calling the earlier `EGL_SYNC_FENCE` fallback jumped through a null
function pointer. Apply [`dawn-switch-fence-fallback.patch`](dawn-switch-fence-fallback.patch)
after the earlier edits. It calls `glFinish` on submission and marks the completed serial
synchronously. This is correct but can reduce frame rate on hardware.

## 5. `src/dawn/native/opengl/SwapChainEGL.cpp` — use the native window dimensions

Apply [`dawn-switch-present.patch`](dawn-switch-present.patch). Mesa allocates its buffers using
the native window dimensions, but its EGL surface reported 0×0 in Ryujinx. Dawn now uses the
`NWindow` dimensions when the EGL query returns zero. The patch also checks `eglSwapBuffers` and
logs its first success. The startup overlay was visible at 1280×720 after this change.

The linked [devkitPro Mesa patch](https://github.com/devkitPro/pacman-packages/blob/master/switch/mesa/switch-mesa-20.1.0-5.patch)
shows how the backend sizes its native buffers.

## Link line

Once Dawn references EGL directly, the linker pulls Mesa's `egl_switch.c`, which needs the nouveau
winsys and `fatalThrow` from libnx. Append both, because `-lnx` is scanned before `-lEGL` and cannot
satisfy it there:

```
-lnx -lEGL -lGLESv2 -lglapi -lstdc++ -lc -lm -lgcc -lsysbase -ldrm_nouveau -lnx
```

## Adapter and startup history

With all of the above:

```
[aurora::gpu] Graphics adapter information
  API: OpenGLES
  Device: NV120 (Unknown)
  Driver: OpenGL version OpenGL ES 3.2 Mesa 20.1.0-rc3 on NV120
[aurora::gpu] Using limits: maxTextureDimension2D: 16384 ...
```

Before the queue fix, device creation stalled after printing the limits.

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

After Aurora initialised, an older build stopped at guest entry resolution:

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

## Symbolized host backtrace at the throw (definitive)

Converting the turn addresses with the image base subtracted (`0x8500000`) makes the throwing path
explicit:

```
__wrap___cxa_throw
std::__throw_system_error(int)
std::_Function_handler<void(), std::call_once<...>
(anonymous namespace)::AdvanceRetrace
(anonymous namespace)::AdvanceDueRetraces
SelectThread_801a9c08
OSSuspendThread_HLE_801aa6a8
InvokeDirectCpu<0x801B5B00>        (guest func_800C9F90 called OSSuspendThread)
func_800C9F90
```

So **`std::call_once` throws `std::system_error` with `ENOSYS`**, from the retrace scheduler, and the
guest has no handler so it becomes `std::terminate`.

`pthread_once` is present in both libnx (`pthread.c`) and libsysbase; defining one in our objects gives
`multiple definition of 'pthread_once'`, so the fault is inside the existing implementation or in how
`__gthread_once` reports it — not a missing symbol. Guarding `OSSuspendThread`'s body with
`catch(std::exception)` does **not** help either: the throw happens further down, inside the retrace
scheduling path reached from it.

Next: determine what libnx's `pthread_once` returns here (log its result before libstdc++ maps it to
`system_error`), and whether a stale `errno` of `ENOSYS` (our `pipe()` shim returns -1 without setting
errno) is being turned into the exception.

## Ruled out: pthread_once and thread TLS

Two hypotheses tested and disproved for the `std::call_once` -> `system_error(ENOSYS)` fault:

* **`pthread_once` failing.** Wrapped with `--wrap=pthread_once` and logged every call:
  **8,525 calls, all `rc=0 errno=0`**, including the ones immediately before the throw. So
  `__gthread_once` is not returning an error, and libstdc++ is calling `__throw_system_error` with a
  value that did not come from it.
* **Secondary threads without TLS.** `SwitchTlsBootstrap()` only ever ran for the main thread, so
  `switch/thread_tls.cpp` now wraps `pthread_create` and gives each new thread a private TLS block
  (`SwitchTlsAttach()`). It is a genuine defect - libnx never writes `tpidr_el0`, so every worker
  thread's thread_locals resolved against address 0 - but fixing it did **not** change this crash.

Next: wrap `std::__throw_system_error` itself and log its argument. That reports the exact errno
libstdc++ decided to throw with, which is the one fact still missing.

## The thrown code is 88, and it is thrown *inside* the call_once callable

Wrapping `std::__throw_system_error` (mangled `_ZSt20__throw_system_errori` - note 20, not 19) and
printing its argument gives:

```
THROW_SYSTEM_ERROR code=88
```

88 is newlib's `ENOSYS`. The backtrace taken at that instant explains why the earlier `pthread_once`
measurement showed rc=0 on every call:

```
__throw_system_error(88)
  <- std::_Function_handler<void(), std::call_once<...>>
    <- pthread_once                 (our wrapper is on the stack; it never returned)
      <- AdvanceRetrace <- AdvanceDueRetraces <- SelectThread <- OSSuspendThread
```

The callable runs *inside* `pthread_once`, so it throws before `pthread_once` can return - which is why
the 8,525 logged calls were all rc=0: the failing one never got that far.

`pthread_create` is also cleared: the wrapper logs failures and none appeared.

So the ENOSYS comes from another pthread primitive used inside the retrace scheduler. libstdc++ turns a
non-zero `pthread_mutex_lock`/`pthread_cond_wait` result into exactly this exception, so those are the
next things to wrap and log.

## Where the ENOSYS hunt stands

Established, each with a measurement rather than an inference:

| Hypothesis | Result |
|---|---|
| `std::__throw_system_error` argument | **88** = newlib `ENOSYS` |
| `pthread_once` returning an error | 8,525 calls, all `rc=0` - the failing call throws *inside* it and never returns |
| `pthread_create` failing in the retrace path | wrapper logs failures; none appeared |
| Guarding `OSSuspendThread` | no effect - throw is deeper |
| Secondary threads lacking TLS | real bug, fixed in `thread_tls.cpp`, but not this crash |

`switch/thread_tls.cpp` ends at the `pthread_create` wrapper (TLS attach + failure logging). A
generated block that wrapped `pthread_mutex_lock`/`pthread_cond_wait`/`pthread_cond_timedwait` was
reverted because the macro failed to emit the definitions and broke the link; redo those as explicit
functions, then rebuild with the matching `--wrap=` flags.

## All pthread primitives cleared

`switch/thread_tls.cpp` now wraps `pthread_create`, `pthread_mutex_lock`,
`pthread_mutex_trylock`, `pthread_cond_wait` and `pthread_cond_timedwait` as explicit functions, each
logging a non-zero return (EBUSY/ETIMEDOUT expected and filtered). Result over a full run:

```
8526 PTHREAD_ONCE        (all rc=0)
0    PTHREAD_CREATE failed
0    mutex/condvar failures
0    any rc!=0
```

So the `std::system_error(88)` does **not** originate in any pthread primitive. Remaining
possibilities: a `filesystem_error` (it derives from `system_error`), or libstdc++ throwing
`system_error(errno)` with a **stale errno of 88** left by one of our shims that returns -1 without
setting errno (`pipe`, `waitpid`, ...).

Next: symbolize frame 2 of the `SETRACE` capture - recompute the image base from the running binary
first, since one frame landed outside the assumed text range - to name the immediate caller of
`__throw_system_error`.

## SOLVED: `pthread_detach` returns ENOSYS, and that was the crash

Wrapping `pthread_detach` gave the answer in one run:

```
PTHREAD pthread_detach rc=88
THROW_SYSTEM_ERROR code=88
```

Horizon's `pthread_detach` is unimplemented (88 = newlib `ENOSYS`), and libstdc++ turns that into
`std::system_error`, which the guest cannot catch - hence "unhandled C++ exception" killed the retrace
scheduler. `__wrap_pthread_detach` now reports success and lets the kernel own the thread.

Result: **zero `THROW_SYSTEM_ERROR`**, `init.log` grows from 1 line to 18,085, and guest dispatch runs:

```
II> 801dbce0
II< resolved
```

Also fixed while chasing it: the log channel retried the host for 20 attempts, and with no listener
each refused connect costs the host's full timeout, delaying startup by ~50 s and making runs look
hung. It now tries twice.

## Next blocker: EGLSurface creation

The renderer now initialises and reaches swapchain setup:

```
[aurora::gpu] Using surface format RGBA8Unorm, present mode Immediate
[aurora::gpu] Render target 640x528 for a 854x480 surface
[aurora::gpu] WebGPU device was lost (Unknown: Couldn't create an EGLSurface for [Surface "Surface"].
```

`eglCreateWindowSurface` needs the NWindow as its native window; the same class of problem as the
`dlopen` fix, one layer further out.

## Surface creation fails with EGL_BAD_NATIVE_WINDOW

Instrumenting the `__SWITCH__` case in `SwapChainEGL.cpp` (log the handle, `nwindowGetDefault()`, and
`eglGetError()`) gives the exact failure:

```
EGL: create window surface
EGL: surface=0x0  window=0xf4169c8  default=0xf4169c8  err=0x300b
```

`0x300b` is **`EGL_BAD_NATIVE_WINDOW`**. The handle is the right one - it equals
`nwindowGetDefault()` - so Aurora's descriptor plumbing is correct and Mesa's Switch EGL is rejecting
it inside `eglCreateWindowSurface`.

Note the TU needs `extern "C" void* nwindowGetDefault(void);` declared locally: Dawn's include path
carries neither libnx headers nor `switch.h`.

Next: read what devkitPro's `egl_switch.c` (inside `libEGL.a`) actually accepts as a native window -
it may require `eglCreatePlatformWindowSurfaceEXT`, a configured NWindow, or a different struct than
`NWindow*`.

## ROOT CAUSE of EGL_BAD_NATIVE_WINDOW: the default NWindow is invalid

Logging `nwindowIsValid()` next to the handle settles it:

```
SDL window: 854x480, native NWindow 0xf4169c8 valid=0
EGL: surface=0x0  err=0x300b (EGL_BAD_NATIVE_WINDOW)
```

`nwindowGetDefault()` returns a pointer, but the object behind it is **not a valid NWindow**, so
Mesa's `switch_create_window_surface` (which calls `nwindowIsValid` before anything else) rejects it.
Dawn and Aurora are both innocent - the handle is passed through correctly.

libnx documents that `nwindowGetDefault()` initialises VI services and creates the window from a
`ViLayer` "automatically during application startup, before main is called". This port overrides
`__appInit`, so that startup path is the likely casualty - the same class of problem as the NPDM
service table.

Next: build the NWindow explicitly (`viInitialize()` +
`appletCreateManagedDisplayLayer()`/`viCreateLayer()` + `nwindowCreateFromLayer()`) and have both the
SDL layer and Aurora's surface descriptor use that handle instead of `nwindowGetDefault()`.
