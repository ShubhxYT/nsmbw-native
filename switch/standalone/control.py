#!/usr/bin/env python3
"""Prepare the title's versioned NACP and HOME icon."""
import sys
from pathlib import Path

from PIL import Image, ImageOps

root = Path("/pack/control_g")
nacp = bytearray(Path("/recipe/switch/nsmbw.nacp").read_bytes())
if len(nacp) != 0x4000:
    sys.exit("Invalid NACP template")
name = b"aaa-nsmbw"
for start in range(0, 0x3000, 0x300):
    if start == 0 or nacp[start]:
        nacp[start : start + 0x200] = name.ljust(0x200, b"\0")
nacp[0x3060:0x3070] = sys.argv[1].encode().ljust(16, b"\0")
nacp[0x3025] = 1  # StartupUserAccount::Required
(root / "control.nacp").write_bytes(nacp)

icon = Path("/run/nsmbw-icon")
if not icon.is_file():
    icon = Path(__file__).with_name("home-icon.png")
with Image.open(icon) as source:
    image = ImageOps.fit(ImageOps.exif_transpose(source).convert("RGB"), (256, 256))
    image.save(root / "icon_AmericanEnglish.dat", "JPEG", quality=90)
