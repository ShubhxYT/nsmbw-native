# Phase 0

The smallest app that proves the Switch toolchain: libnx, framebuffer, pad, `.nro` on the console.

```bash
docker build -t nsmbw-switch ../          # once
docker run --rm -v "$PWD":/src -w /src nsmbw-switch bash -lc make
cp nsmbw-check.nro /Volumes/<SD>/switch/  # then launch from hbmenu
```

Expected on screen:

```
NSMBW native - Switch toolchain check

firmware:      19.0.0
operation:     handheld
applet type:   applet (hbmenu)
target:        1280x720 (Switch Lite handheld)
pad:           read ok
```

`+` exits.
