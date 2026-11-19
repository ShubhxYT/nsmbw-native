#!/usr/bin/env python3
"""Terminate an ELF's .dynamic section.

The link produces a .dynamic list that ends at DT_RELACOUNT with no DT_NULL, because libnx's specs
pass --spare-dynamic-tags=0. libnx's __nx_dynamic loops until it sees DT_NULL, so without one it walks
off the section into adjacent data — the crash we saw inside _dynProcessRela/__nx_dynamic.

DT_RELACOUNT is advisory (libnx uses RELASZ), so its 16-byte slot becomes the terminator. Everything
libnx reads — DT_RELA, DT_RELASZ, DT_RELAENT — is earlier in the list and untouched.
"""
import struct
import sys


def main(path: str) -> None:
    data = bytearray(open(path, "rb").read())

    # Locate .dynamic through the ELF section headers.
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

    def section(index):
        off = e_shoff + index * e_shentsize
        name, kind, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, off)
        return name, kind, addr, offset, size

    _, _, _, strtab_off, strtab_size = section(e_shstrndx)
    strtab = data[strtab_off:strtab_off + strtab_size]

    dynamic = None
    for index in range(e_shnum):
        name, kind, addr, offset, size = section(index)
        if strtab[name:].split(b"\0")[0] == b".dynamic":
            dynamic = (offset, size)
            break
    if dynamic is None:
        sys.exit("no .dynamic section")

    offset, size = dynamic
    entries = size // 16
    last_tag, last_val = struct.unpack_from("<QQ", data, offset + (entries - 1) * 16)
    print(f".dynamic: {entries} entries, last tag {last_tag:#x}")

    if last_tag == 0:
        print("already terminated")
        return
    if last_tag != 0x6FFFFFF9:
        sys.exit(f"last tag {last_tag:#x} is not DT_RELACOUNT; refusing to overwrite")

    struct.pack_into("<QQ", data, offset + (entries - 1) * 16, 0, 0)
    open(path, "wb").write(data)
    print("DT_RELACOUNT slot replaced with DT_NULL")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "/src/wiicompiled.elf")
