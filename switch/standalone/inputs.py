#!/usr/bin/env python3
"""Validate the disc header and fingerprint the inputs to translation."""
import hashlib
import sys
from pathlib import Path


def hash_file(path, digest):
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)


def main():
    action = sys.argv[1]
    digest = hashlib.sha256()
    if action == "hash":
        hash_file(Path(sys.argv[2]), digest)
    elif action == "validate":
        disc, upstream, *sources = sys.argv[2:]
        disc = Path(disc)
        header = disc / "sys/boot.bin"
        if not header.is_file() or not (disc / "files").is_dir():
            sys.exit("Expected an extracted Wii disc with sys/ and files/.")
        with header.open("rb") as source:
            boot = source.read(0x20)
        if boot[:6] != b"SMNE01" or len(boot) != 0x20 or boot[7] != 1:
            sys.exit("This build supports the USA SMNE01 revision 1 disc.")
        digest.update(upstream.encode())
        for source in sources:
            hash_file(Path(source), digest)
        for name in ("sys/boot.bin", "sys/main.dol", "sys/fst.bin",
                     "files/rels/d_profileNP.rel.LZ", "files/rels/d_basesNP.rel.LZ",
                     "files/rels/d_enemiesNP.rel.LZ", "files/rels/d_en_bossNP.rel.LZ"):
            path = disc / name
            if not path.is_file():
                sys.exit(f"Missing disc file {name}")
            hash_file(path, digest)
    else:
        sys.exit(f"Unknown action {action}")
    print(digest.hexdigest())


if __name__ == "__main__":
    main()
