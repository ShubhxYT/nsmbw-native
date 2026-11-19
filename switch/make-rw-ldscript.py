#!/usr/bin/env python3
"""Generate switch-rw.ld: devkitPro's switch.ld with .rodata placed in the writable segment.

A title installed from an NSP maps .rodata read-only, and devkitPro's prebuilt libraries put vtables,
locale tables and other relocated data there, so every R_AARCH64_RELATIVE store into them faults
(Atmosphere reports it as "Fault Address: 0"). Putting the .rodata output section in the :data region
makes it writable and removes the need to rewrite the libraries at all.

The block has to move to the end of the read-only group rather than being retargeted in place:
sections that follow it (.tls.align, .eh_frame, .gnu.version*, .note.gnu.build-id) stay in :rodata,
and a :rodata section placed after a :data one produces overlapping LMAs. The read-only segment
therefore keeps the dynamic tables and the unwind tables, which keeps elf2nso's three-loadable-segment
requirement satisfied.
"""
import pathlib

SOURCE = pathlib.Path("/opt/devkitpro/libnx/switch.ld")
TARGET = pathlib.Path("/src/switch/switch-rw.ld")

BLOCK = """\t.rodata :
\t{
\t\t*(.rodata .rodata.* .gnu.linkonce.r.*)
\t\t. = ALIGN(8);
\t} :rodata

"""

RELRO_MARKER = "\t/* =========== RELRO section =========== */"

MOVED = """\t/* NSMBW: .rodata lives in the writable segment. A title installed from an NSP has no writable
\t   .rodata, and devkitPro's prebuilt libraries put vtables and locale tables there, so every
\t   R_AARCH64_RELATIVE store into them faults. */
\t. = ALIGN(0x1000);
\t.rodata :
\t{
\t\t*(.rodata .rodata.* .gnu.linkonce.r.*)
\t\t. = ALIGN(8);
\t} :data

"""


def main() -> None:
    text = SOURCE.read_text()
    if BLOCK not in text:
        raise SystemExit("switch.ld: .rodata block not found - toolchain layout changed")
    if RELRO_MARKER not in text:
        raise SystemExit("switch.ld: RELRO marker not found - toolchain layout changed")
    text = text.replace(BLOCK, "", 1)
    text = text.replace(RELRO_MARKER, MOVED + RELRO_MARKER, 1)
    TARGET.write_text(text)
    print(f"wrote {TARGET}")
    write_specs()





def write_specs() -> None:
    """A specs file whose -T points at switch-rw.ld. Passing a second -T on the command line leaves
    both scripts in effect, which resets the location counter and produces overlapping LMAs."""
    specs = pathlib.Path("/opt/devkitpro/libnx/switch.specs").read_text()
    old = "%:getenv(DEVKITPRO /libnx/switch.ld)"
    if old not in specs:
        raise SystemExit("switch.specs: -T path not found - toolchain layout changed")
    target = pathlib.Path("/src/switch/switch-rw.specs")
    target.write_text(specs.replace(old, "/src/switch/switch-rw.ld", 1))
    print(f"wrote {target}")


if __name__ == "__main__":
    main()
