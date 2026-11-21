#!/usr/bin/env bash
# Run with the staged source at /src and the checkout containing this recipe at /recipe.
set -euo pipefail
recipe=/recipe/switch
jobs=${NSMBW_BUILD_JOBS:-4}
export NSMBW_SWITCH_BACKEND=deko3d NSMBW_LOG_HOST= NAME=aaa-nsmbw

case "$1" in
    compile)
        bash "$recipe/standalone/dependencies.sh"
        mkdir -p /src/switch/include
        cp -p "$recipe"/*.{cpp,h,py,sh,S,json} /src/switch/
        cp -pR "$recipe"/include/. /src/switch/include/
        python3 /src/switch/make-rw-ldscript.py
        python3 "$recipe/standalone/compile.py"
        ninja -C /src -f switch-objects.ninja -j "$jobs"
        ;;
    pack)
        version=$2
        mkdir -p /pack/{exefs,control_g,romfs/game,romfs/wii_bootstrap/shared2}
        cp -R /disc/sys /disc/files /pack/romfs/game/
        assets=/src/runtime/assets
        cp "$assets/dsp/dsp_coef.bin" "$assets/pipeline/initial_pipeline_cache.db" /pack/romfs/
        cp -R "$assets/wii/shared2/wc24" /pack/romfs/wii_bootstrap/shared2/
        python3 "$recipe/standalone/control.py" "$version"
        NSMBW_SWITCH_KEYS=/run/nsmbw-keys bash /src/switch/rebuild.sh --pack
        ;;
    *) exit 2 ;;
esac
