#!/usr/bin/env python3
"""Describe the Switch platform, game and dependency objects for Ninja."""
import os
import shlex
from pathlib import Path

SRC = Path("/src")
DEPS = Path("/deps")
DEVKIT = Path(os.environ.get("DEVKITPRO", "/opt/devkitpro"))
COMPILER = DEVKIT / "devkitA64/bin/aarch64-none-elf"
OUT = SRC / "obj-pie"
RUNTIME = SRC / "runtime"
AURORA = SRC / "aurora-main"
GENERATED = SRC / "generated"

SYSTEM = [
    DEVKIT / "libnx/include",
    DEVKIT / "portlibs/switch/include",
    SRC / "switch/include",
    RUNTIME / "third_party",
    RUNTIME / "third_party/toml11",
    RUNTIME / "third_party/pugixml",
    AURORA / "include",
    AURORA / "lib",
    DEPS / "SDL/include",
    DEPS / "imgui",
    DEPS / "fmt/include",
    DEPS / "xxhash",
    DEPS / "tracy/public",
    DEPS / "absl",
    DEPS / "sqlite",
    DEPS / "png",
    DEPS / "zlib",
    Path("/deko/dawn/include"),
    Path("/deko/dawn-build/gen/include"),
]
BASE = ["-D__SWITCH__", "-DTARGET_PC", "-DCRYPTOPP_DISABLE_ASM"]
CXX20 = ["-std=gnu++20", "-fexceptions", "-frtti"]
INCLUDES = [f"-I{p}" for p in (SRC, RUNTIME / "include", RUNTIME / "src")]
for directory in SYSTEM:
    INCLUDES.extend(("-isystem", str(directory)))

lines = [
    "ninja_required_version = 1.3",
    "rule cxx",
    f"  command = {COMPILER}-g++ -fPIE -O2 $flags -MD -MF $out.d -c $in -o $out",
    "  description = CXX $out",
    "  depfile = $out.d",
    "  deps = gcc",
    "rule cc",
    f"  command = {COMPILER}-gcc -fPIE -O2 $flags -MD -MF $out.d -c $in -o $out",
    "  description = CC $out",
    "  depfile = $out.d",
    "  deps = gcc",
]
objects = []


def add(source, name, flags=()):
    if not source.is_file():
        raise SystemExit(f"missing build input {source}")
    output = OUT / name
    output.parent.mkdir(parents=True, exist_ok=True)
    rule = "cc" if source.suffix in (".c", ".S") else "cxx"
    lines.append(f"build {output}: {rule} {source}")
    lines.append("  flags = " + " ".join(shlex.quote(str(flag)) for flag in flags))
    objects.append(str(output))


def units(root, pattern, subdir, prefix, flags, exclude=()):
    for source in sorted(root.glob(pattern)):
        rel = source.relative_to(root).as_posix()
        if rel not in exclude:
            name = prefix + rel.replace("/", "_") + ".o"
            add(source, Path(subdir) / name, flags)


runtime_flags = CXX20 + BASE + INCLUDES
units(RUNTIME / "src", "**/*.cpp", "", "", runtime_flags + ["-DNSMBW_USE_DEKO3D"], (
    "discord_presence.cpp", "guest_flat_memory_macos.cpp", "hd_texture_pack.cpp",
))
units(AURORA / "lib", "**/*.cpp", "aurora", "aur_", runtime_flags + [
    "-DAURORA_ENABLE_GX", "-DWEBGPU_DAWN", "-DDAWN_ENABLE_BACKEND_DEKO3D", "-DNSMBW_USE_DEKO3D",
], ("dolphin/AR.cpp", "dolphin/card.cpp", "dolphin/dvd/dvd.cpp", "main.cpp"))

for name in ("sdl_switch", "switch_appinit", "absl_horizon", "switch_shims", "thread_tls",
             "cxa_throw_probe", "log_channel", "discord_stub", "hd_texture_stub"):
    add(SRC / "switch" / f"{name}.cpp", f"{name}.o", runtime_flags + [
        "-DNSMBW_USE_DEKO3D", '-DNSMBW_LOG_HOST=""', "-I/src/switch", "-I/src/switch/include",
    ])
add(RUNTIME / "src/platform/android/co_switch.S", "co_switch.o")
add(GENERATED / "data_sections_init_blobs.S", "gen/data_blobs.o")
units(GENERATED, "*.cpp", "gen", "gen_", runtime_flags)
for group, subdir, name, flags in (
    ("base_common", "game", "game_", ["-std=gnu++20"] + BASE + INCLUDES),
    ("base_registration", "gen", "reg_", runtime_flags),
):
    shards = sorted((GENERATED / "build_shards" / group).glob("*.cpp"))
    if not shards:
        raise SystemExit(f"translator did not emit any {group} shards")
    for shard in shards:
        add(shard, f"{subdir}/{name}{shard.stem}.o", flags)

third = RUNTIME / "third_party"
for source in sorted((third / "cryptopp").glob("*.cpp")):
    if not any(word in source.name for word in ("test", "bench", "datatest", "dlltest", "validat", "regtest")):
        add(source, f"lib/cryptopp_{source.stem}.o", [
            "-std=gnu++17", "-DCRYPTOPP_DISABLE_ASM", "-DCRYPTOPP_NO_GLOBAL_BYTE", f"-I{third}",
        ])
add(third / "pugixml/pugixml.cpp", "lib/pugixml.o", [f"-I{third}/pugixml"])
add(DEPS / "xxhash/xxhash.c", "lib/xxhash.o", ["-I/deps/xxhash"])
for name in ("format", "os"):
    add(DEPS / "fmt/src" / f"{name}.cc", f"fmt/{name}.o", ["-std=gnu++17", "-I/deps/fmt/include"])

imgui_flags = ["-std=gnu++17", '-DIMGUI_USER_CONFIG="aurora/imgui_config.h"'] + INCLUDES
for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets"):
    add(DEPS / "imgui" / f"{name}.cpp", f"lib/{name}.o", imgui_flags)
for name in ("imgui_impl_sdl3", "imgui_impl_sdlrenderer3", "imgui_impl_wgpu"):
    add(DEPS / "imgui/backends" / f"{name}.cpp", f"lib/{name}.o", imgui_flags + [
        "-DIMGUI_IMPL_WEBGPU_BACKEND_DAWN", "-I/deps/imgui/backends",
    ])

absl_exclude = (
    "failure_signal_handler", "elf_mem_image", "examine_stack", "stack_consumption", "vdso_support",
    "scoped_mock_log", "status_matchers", "print_hash_of", "gaussian_distribution_gentables",
)
for source in sorted((DEPS / "absl/absl").rglob("*.cc")):
    relative = source.relative_to(DEPS / "absl").as_posix()
    if not any(word in relative for word in ("test", "benchmark")) and source.stem not in absl_exclude:
        name = "absl_" + relative.replace("/", "_") + ".o"
        add(source, f"absl/{name}", [
            "-std=gnu++17", "-D__SWITCH__", "-fexceptions", "-frtti", "-I/deps/absl",
            "-isystem", "/src/switch/include", "-include", "/src/switch/include/switch_posix.h",
        ])
for source in sorted((DEPS / "mbedtls/library").glob("*.c")):
    platform_flags = ["-D__unix__"] if source.stem in ("entropy_poll", "net_sockets", "timing") else []
    add(source, f"mbedtls/{source.stem}.o", [
        "-DMBEDTLS_PLATFORM_MS_TIME_ALT", "-I/deps/mbedtls/include", "-I/deps/mbedtls/library",
        "-isystem", str(DEVKIT / "libnx/include"), "-isystem", "/src/switch/include",
    ] + platform_flags)
for name in ("adler32", "compress", "crc32", "deflate", "gzclose", "gzlib", "gzread", "gzwrite",
             "infback", "inffast", "inflate", "inftrees", "trees", "uncompr", "zutil"):
    add(DEPS / "zlib" / f"{name}.c", f"sys/zlib_{name}.o", ["-I/deps/zlib", "-DHAVE_UNISTD_H"])
for name in ("png", "pngerror", "pngget", "pngmem", "pngpread", "pngread", "pngrio", "pngrtran",
             "pngrutil", "pngset", "pngtrans", "pngwio", "pngwrite", "pngwtran", "pngwutil",
             "arm/arm_init", "arm/filter_neon_intrinsics", "arm/palette_neon_intrinsics"):
    add(DEPS / "png" / f"{name}.c", "sys/png_" + name.replace("/", "_") + ".o",
        ["-I/deps/png", "-I/deps/zlib"])
add(DEPS / "png/arm/filter_neon.S", "sys/png_arm_filter_neon.o", ["-I/deps/png", "-I/deps/zlib"])
add(DEPS / "sqlite/sqlite3.c", "sys/sqlite3.o",
    ["-std=gnu11", "-DSQLITE_OS_OTHER=1", "-DSQLITE_TEMP_STORE=3", "-DSQLITE_OMIT_LOAD_EXTENSION"])
add(DEPS / "sqlite/nx-vfs.c", "sys/sqlite_nx_vfs.o",
    ["-std=gnu11", "-I/deps/sqlite", "-isystem", str(DEVKIT / "libnx/include")])

lines.append("build objects: phony " + " ".join(objects))
lines.append("default objects\n")
target = SRC / "switch-objects.ninja"
contents = "\n".join(lines)
if not target.exists() or target.read_text() != contents:
    target.write_text(contents)
