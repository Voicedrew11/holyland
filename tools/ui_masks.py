#!/usr/bin/env python3
"""The player menu's pictures (port/ui/art/*.png, white on clear) as the alpha masks the game carries
(port/ui/res/*.alpha), so the game needs no PNG decoder: "KF4A", then the width and the height as
little-endian 32-bit numbers, then a byte of alpha for each pixel, rows top to bottom. The menu's sheet
colours them (image-color). Run after changing a picture (tools/ui_ornaments.py draws the ornaments); the masks are
committed. Needs Pillow."""
import struct
import sys
from pathlib import Path

from PIL import Image

# (picture in port/ui/art/, mask in port/ui/res/, width in pixels). A width crops the picture to what is drawn and
# scales it to that width; None keeps the picture as drawn. About 3 pixels a dp, so sharp up to 4K.
MASKS = [
    ("holyland.png", "logo.alpha", 720),  # the sidebar's logo, about 230 dp wide
    ("corner_tl.png", "corner_tl.alpha", None),
    ("corner_tr.png", "corner_tr.alpha", None),
    ("corner_bl.png", "corner_bl.alpha", None),
    ("corner_br.png", "corner_br.alpha", None),
    ("soft_box.png", "soft_box.alpha", None),
]


def make_mask(source: Path, target: Path, width: int | None) -> bool:
    alpha = Image.open(source).convert("RGBA").getchannel("A")
    if width is not None:
        box = alpha.getbbox()
        if box is None:
            print(f"{source}: nothing drawn", file=sys.stderr)
            return False
        alpha = alpha.crop(box)
        height = round(alpha.height * width / alpha.width)
        alpha = alpha.resize((width, height), Image.Resampling.LANCZOS)
    target.write_bytes(b"KF4A" + struct.pack("<II", alpha.width, alpha.height) + alpha.tobytes())
    print(f"{target}: {alpha.width} x {alpha.height}")
    return True


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    ok = all([make_mask(root / "port/ui/art" / picture, root / "port/ui/res" / mask, width)
              for picture, mask, width in MASKS])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
