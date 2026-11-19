#!/usr/bin/env python3
"""Recompile a single Dawn translation unit in place and replace it in libwebgpu_dawn.a.

Used to apply small Switch-specific patches without rebuilding all of Dawn (~15 minutes): the
compile command comes from Dawn's own compile_commands.json, so the flags match the original build.
"""
import json
import os
import shlex
import subprocess
import sys

DAWN = "/deps/dawn-src"
BUILD = os.path.join(DAWN, "build-switch")
LIB = os.path.join(BUILD, "src/dawn/native/libwebgpu_dawn.a")


def main() -> None:
    target = sys.argv[1] if len(sys.argv) > 1 else "DisplayEGL.cpp"
    entries = json.load(open(os.path.join(BUILD, "compile_commands.json")))
    entry = next((e for e in entries if e["file"].endswith(target)), None)
    if entry is None:
        raise SystemExit(f"no compile command for {target}")

    argv = shlex.split(entry["command"])
    if "-o" not in argv:
        raise SystemExit("compile command has no -o")
    # The -o path in compile_commands.json is relative to the build directory; ar runs from here, so
    # resolve it now. Passing the relative path silently achieved nothing the first time.
    obj = os.path.join(entry["directory"], argv[argv.index("-o") + 1])

    print(f"compiling {target} -> {obj}")
    result = subprocess.run(argv, cwd=entry["directory"], capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout[-2000:])
        print(result.stderr[-2000:])
        raise SystemExit("compile failed")

    ar = subprocess.run(["ar", "r", LIB, obj], capture_output=True, text=True)
    if ar.returncode != 0:
        print(ar.stdout[-800:])
        print(ar.stderr[-800:])
        raise SystemExit("ar r failed")
    ranlib = subprocess.run(["ar", "s", LIB], capture_output=True, text=True)
    if ranlib.returncode != 0:
        raise SystemExit("ar s failed")
    print("archive size:", os.path.getsize(LIB))


if __name__ == "__main__":
    main()
