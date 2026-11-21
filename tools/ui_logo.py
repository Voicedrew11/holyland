#!/usr/bin/env python3
"""The player menu's logo (port/ui/art/holyland.png, white on clear) as the alpha mask the game carries
(port/ui/res/logo.alpha), so the game needs no PNG decoder: "KF4A", then the width and the height as
little-endian 32-bit numbers, then a byte of alpha for each pixel, rows top to bottom. The menu's sheet
colours it (image-color). Run after changing the picture; the mask is committed. Needs Pillow."""
import struct
import sys
from pathlib import Path

from PIL import Image

WIDTH = 720  # pixels: the sidebar's logo is about 230 dp wide, drawn sharp up to about 3 pixels a dp (4K)


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    source = root / "port/ui/art/holyland.png"
    target = root / "port/ui/res/logo.alpha"
    alpha = Image.open(source).convert("RGBA").getchannel("A")
    box = alpha.getbbox()
    if box is None:
        print(f"{source}: nothing drawn", file=sys.stderr)
        return 1
    alpha = alpha.crop(box)
    height = round(alpha.height * WIDTH / alpha.width)
    alpha = alpha.resize((WIDTH, height), Image.Resampling.LANCZOS)
    target.write_bytes(b"KF4A" + struct.pack("<II", WIDTH, height) + alpha.tobytes())
    print(f"{target}: {WIDTH} x {height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
