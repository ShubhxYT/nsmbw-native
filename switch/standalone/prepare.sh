#!/usr/bin/env bash
# Shared disc extraction and translation for the host and container builders.

# Reads root, out, disc, disc_dir, nod_asset and jobs. Sets source_dir and disc_dir.
prepare_switch_source() {
    local disc_hash nod extracted build_id
    local upstream=e5fce0f26a2229b8ed70357d2d503780235c0eba
    if [[ -n "$disc" ]]; then
        step 'Extracting your disc'
        disc_hash="$(python3 "$root/switch/standalone/inputs.py" hash "$disc")"
        disc_dir="$out/discs/$disc_hash"
        if [[ ! -f "$disc_dir/.extracted" ]]; then
            nod="$out/tools/nodtool-$nod_asset"
            if [[ ! -x "$nod" ]]; then
                curl -fLSs --retry 3 "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-$nod_asset" -o "$nod.tmp"
                chmod +x "$nod.tmp"
                mv "$nod.tmp" "$nod"
            fi
            [[ ! -e "$disc_dir" ]] || die "incomplete extraction at $disc_dir (choose another --out)"
            mkdir -p "$out/discs"
            extracted="$(mktemp -d "$out/discs/.extract.XXXXXX")"
            if ! "$nod" extract "$disc" "$extracted" >"$out/logs/extract.log" 2>&1; then
                rm -rf "$extracted"
                die "disc extraction failed (see $out/logs/extract.log)"
            fi
            touch "$extracted/.extracted"
            mv "$extracted" "$disc_dir"
        fi
    fi

    build_id="$(python3 "$root/switch/standalone/inputs.py" validate "$disc_dir" \
        "$upstream" "$root/switch/patches/standalone-runtime.patch" \
        "$root/switch/standalone/translate.sh" "$root/switch/standalone/global.json")"
    source_dir="$out/source-$build_id"
    if [[ ! -f "$source_dir/.translated" ]]; then
        step 'Fetching the pinned runtime and translating the disc'
        if [[ ! -d "$source_dir/.git" ]]; then
            git init -q "$source_dir"
        fi
        git -C "$source_dir" fetch -q --depth 1 \
            https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation.git "$upstream"
        git -C "$source_dir" checkout -q --detach --force "$upstream"
        git -C "$source_dir" apply --whitespace=nowarn "$root/switch/patches/standalone-runtime.patch"
        cp "$root/switch/standalone/global.json" "$source_dir/global.json"
        bash "$root/switch/standalone/translate.sh" "$source_dir" "$disc_dir" "$root" "$jobs" \
            >"$out/logs/translate.log" 2>&1 ||
            { tail -16 "$out/logs/translate.log" >&2; die 'translation failed'; }
        touch "$source_dir/.translated"
    fi
}
