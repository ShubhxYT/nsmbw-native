#!/usr/bin/env bash
# Prepare the native renderer, third-party sources and host packager.
set -euo pipefail
patches=/recipe/switch/patches
jobs=${NSMBW_BUILD_JOBS:-4}
devkit=${DEVKITPRO:-/opt/devkitpro}

checkout() {
    local dir=$1 url=$2 revision=$3
    if [[ -f "$dir/.nsmbw-prepared" ]]; then return; fi
    git config --global --add safe.directory "$dir"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$revision"
    git -C "$dir" checkout -q --detach --force FETCH_HEAD
}
apply_patch() {
    local dir=$1 patch=$2
    shift 2
    if ! git -C "$dir" apply --reverse --check "$@" "$patch" >/dev/null 2>&1; then
        git -C "$dir" apply --whitespace=nowarn "$@" "$patch"
    fi
}
download() {
    local url=$1 target=$2
    if [[ ! -f "$target" ]]; then
        curl -fLSs --retry 3 "$url" -o "$target.tmp"
        mv "$target.tmp" "$target"
    fi
}
archive() {
    local dir=$1 url=$2
    if [[ -f "$dir/.nsmbw-prepared" ]]; then return; fi
    mkdir -p "$dir"
    download "$url" "$dir/source.tar.gz"
    tar -xzf "$dir/source.tar.gz" --strip-components=1 -C "$dir"
    touch "$dir/.nsmbw-prepared"
}

checkout /deps/SDL https://github.com/libsdl-org/SDL.git release-3.4.4
checkout /deps/absl https://github.com/abseil/abseil-cpp.git 4447c7562e3bc702ade25105912dce503f0c4010
checkout /deps/imgui https://github.com/ocornut/imgui.git 4806a1924ff6181180bf5e4b8b79ab4394118875
checkout /deps/fmt https://github.com/fmtlib/fmt.git 123913715afeb8a437e6388b4473fcc4753e1c9a
checkout /deps/xxhash https://github.com/Cyan4973/xxHash.git 2ec67caeb18ec15ae60c0853174331089acddfc0
checkout /deps/tracy https://github.com/wolfpld/tracy.git 5d542dc09f3d9378d005092a4ad446bd405f819a
checkout /deps/hacBrewPack https://github.com/rlaphoenix/hacBrewPack.git 745b16ecfc9ce055743067d200572204cb2aac6c
apply_patch /deps/absl "$patches/standalone-abseil.patch"
for dir in SDL absl imgui fmt xxhash tracy hacBrewPack; do touch "/deps/$dir/.nsmbw-prepared"; done

archive /deps/png https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.44
archive /deps/zlib https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1
archive /deps/mbedtls https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/mbedtls-3.6.7
[[ -f /deps/png/pnglibconf.h ]] || cp /deps/png/scripts/pnglibconf.h.prebuilt /deps/png/pnglibconf.h
mkdir -p /deps/sqlite
if [[ ! -f /deps/sqlite/sqlite3.c ]]; then
    download https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip /deps/sqlite/source.zip
    unzip -jo /deps/sqlite/source.zip '*/sqlite3.c' '*/sqlite3.h' -d /deps/sqlite
fi
cp -p /recipe/switch/standalone/nx-vfs.c /deps/sqlite/nx-vfs.c

if [[ ! -x /deps/hacBrewPack/hacbrewpack ]]; then
    cp /deps/hacBrewPack/config.mk.template /deps/hacBrewPack/config.mk
    make -C /deps/hacBrewPack -j "$jobs"
fi

checkout /deko/uam https://github.com/averne/libuam.git f8c9eef01ffe06334d530393d636d69e2b52744b
apply_patch /deko/uam "$patches/standalone-uam.patch"
touch /deko/uam/.nsmbw-prepared
if [[ ! -f /deko/uam/build-switch/build.ninja ]]; then
    cat > /deko/uam-switch.cross <<CROSS
[built-in options]
buildtype = 'release'
default_library = 'static'
wrap_mode = 'nodownload'
[binaries]
c = '$devkit/devkitA64/bin/aarch64-none-elf-gcc'
cpp = '$devkit/devkitA64/bin/aarch64-none-elf-g++'
ar = '$devkit/devkitA64/bin/aarch64-none-elf-ar'
strip = '$devkit/devkitA64/bin/aarch64-none-elf-strip'
pkg-config = '$devkit/portlibs/switch/bin/aarch64-none-elf-pkg-config'
[host_machine]
system = 'hos'
cpu_family = 'aarch64'
cpu = 'armv8a'
endian = 'little'
CROSS
    meson setup /deko/uam/build-switch /deko/uam --cross-file /deko/uam-switch.cross
fi
ninja -C /deko/uam/build-switch -j "$jobs" libuam.a

if [[ ! -f /deko/dawn/.nsmbw-prepared ]]; then
    checkout /deko/dawn https://github.com/google/dawn.git 13abc3bc8ea2d3c2050f9e77a12d012108ceee24
    # Dawn's submodule URLs include absolute Chromium-hosted repositories.
    git -C /deko/dawn submodule update --init --depth 1 -- \
        third_party/abseil-cpp third_party/jinja2 third_party/markupsafe third_party/spirv-headers/src
    url=https://raw.githubusercontent.com/schuler-ph/nsmbw-switch/502d87fc10111017dbaeb354738bcd4f2f8474db
    download "$url/aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch" /deko/dawn-platform.patch
    echo '7d678de6d7dcb95a251702e079b36dbe8897ef5bc5b68e27bef435c885640138  /deko/dawn-platform.patch' | sha256sum -c -
    git -C /deko/dawn apply --whitespace=nowarn --exclude='third_party/*' /deko/dawn-platform.patch
    apply_patch /deko/dawn "$patches/standalone-dawn.patch"
    apply_patch /deko/dawn/third_party/abseil-cpp "$patches/standalone-dawn-abseil.patch"
    touch /deko/dawn/.nsmbw-prepared
fi
if [[ ! -f /deko/dawn-build/build.ninja ]]; then
    cmake -G Ninja -S /deko/dawn -B /deko/dawn-build \
        -DCMAKE_TOOLCHAIN_FILE="$devkit/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC -DDAWN_ENABLE_PIC=ON \
        -DDAWN_ENABLE_DEKO3D=ON -DDAWN_UAM_DIR=/deko/uam \
        -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF \
        -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF \
        -DDAWN_ENABLE_SPIRV_VALIDATION=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_SAMPLES=OFF \
        -DDAWN_FETCH_DEPENDENCIES=OFF -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_BUILD_NODE_BINDINGS=OFF \
        -DDAWN_ENABLE_SWIFTSHADER=OFF -DTINT_BUILD_IR_BINARY=OFF -DTINT_BUILD_TINTD=OFF \
        -DDAWN_USE_GLFW=OFF -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF \
        -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_WGSL_READER=ON \
        -DTINT_BUILD_GLSL_WRITER=ON -DTINT_BUILD_WGSL_WRITER=ON -DTINT_BUILD_HLSL_WRITER=OFF \
        -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=OFF
fi
cmake --build /deko/dawn-build --target webgpu_dawn --parallel "$jobs"
