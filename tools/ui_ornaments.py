#!/usr/bin/env python3
"""Draws the player menu's ornaments (white on clear) into port/ui/art/: the window's four corners, the two rules
under the titles and the soft box the sidebar's shade is cut from. tools/ui_masks.py then makes the alpha masks the game carries. Run after changing a drawing here;
the pictures and the masks are committed. Needs Pillow."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

SCALE = 4  # drawn this many times larger, then shrunk, for smooth edges


def diamond(draw: ImageDraw.ImageDraw, x: float, y: float, r: float) -> None:
    draw.polygon([(x, y - r), (x + r, y), (x, y + r), (x - r, y)], fill=255)


def faded_line(image: Image.Image, x0: int, x1: int, y: int, half: int, fade_px: int, fade_left: bool,
               fade_right: bool) -> None:
    """A horizontal line from x0 to x1, its ends fading out over fade_px."""
    for x in range(x0, x1):
        a = 1.0
        if fade_left:
            a = min(a, (x - x0) / fade_px)
        if fade_right:
            a = min(a, (x1 - x) / fade_px)
        value = round(255 * max(0.0, min(1.0, a)))
        for dy in range(-half, half + 1):
            image.putpixel((x, y + dy), max(image.getpixel((x, y + dy)), value))


def corner_top_left(size: int) -> Image.Image:
    """An iron bracket: two arms along the frame, a second thinner arm inside, a diamond on the corner and a stud at
    each arm's end."""
    s = size * SCALE
    image = Image.new("L", (s, s), 0)
    draw = ImageDraw.Draw(image)
    w = 3 * SCALE  # the outer arms' thickness
    draw.rectangle([0, 0, s - 6 * SCALE, w - 1], fill=255)
    draw.rectangle([0, 0, w - 1, s - 6 * SCALE], fill=255)
    inner = 7 * SCALE
    t = 1 * SCALE
    draw.rectangle([inner, inner, s * 0.62, inner + t], fill=255)
    draw.rectangle([inner, inner, inner + t, s * 0.62], fill=255)
    diamond(draw, inner + t / 2, inner + t / 2, 6 * SCALE)
    end = s - 3 * SCALE
    diamond(draw, end, w / 2, 3 * SCALE)
    diamond(draw, w / 2, end, 3 * SCALE)
    return image.resize((size, size), Image.Resampling.LANCZOS)


def rule_centred(width: int, height: int) -> Image.Image:
    """A thin rule fading out at both ends, a diamond and two small ones in its middle."""
    w, h = width * SCALE, height * SCALE
    image = Image.new("L", (w, h), 0)
    mid = h // 2
    faded_line(image, 0, w, mid, SCALE // 2, w // 3, True, True)
    draw = ImageDraw.Draw(image)
    draw.rectangle([w // 2 - 18 * SCALE, mid - 3 * SCALE, w // 2 + 18 * SCALE, mid + 3 * SCALE], fill=0)
    diamond(draw, w / 2, mid, h / 2 - SCALE)
    diamond(draw, w / 2 - 13 * SCALE, mid, 3.5 * SCALE)
    diamond(draw, w / 2 + 13 * SCALE, mid, 3.5 * SCALE)
    return image.resize((width, height), Image.Resampling.LANCZOS)


def rule_from_left(width: int, height: int) -> Image.Image:
    """A diamond at the left, then a rule fading out to the right."""
    w, h = width * SCALE, height * SCALE
    image = Image.new("L", (w, h), 0)
    mid = h // 2
    start = h + 4 * SCALE
    faded_line(image, start, w, mid, SCALE // 2, w * 2 // 3, False, True)
    draw = ImageDraw.Draw(image)
    diamond(draw, h / 2, mid, h / 2 - SCALE)
    return image.resize((width, height), Image.Resampling.LANCZOS)


def soft_box(fade: int, middle: int) -> Image.Image:
    """A square, whole in its middle, fading smoothly to nothing over `fade` pixels on each side (its corners in both
    directions): drawn as a nine-patch, whose sides keep their width however large the box."""
    side = 2 * fade + middle
    image = Image.new("L", (side, side), 0)

    def reach(i: int) -> float:  # 0 at the outside to 1 inside the fade, smoothed
        t = min(1.0, (min(i, side - 1 - i) + 0.5) / fade)
        return t * t * (3 - 2 * t)

    image.putdata([round(255 * reach(x) * reach(y)) for y in range(side) for x in range(side)])
    return image


def main() -> int:
    art = Path(__file__).resolve().parent.parent / "port/ui/art"
    # sizes in pixels: about 3 pixels a dp, so sharp up to 4K
    corner = corner_top_left(192)
    pictures = {
        "corner_tl.png": corner,
        "corner_tr.png": corner.transpose(Image.Transpose.FLIP_LEFT_RIGHT),
        "corner_bl.png": corner.transpose(Image.Transpose.FLIP_TOP_BOTTOM),
        "corner_br.png": corner.transpose(Image.Transpose.ROTATE_180),
        "rule_centred.png": rule_centred(660, 36),
        "rule_left.png": rule_from_left(1260, 24),
        "soft_box.png": soft_box(64, 8),
    }
    for name, alpha in pictures.items():
        white = Image.new("RGBA", alpha.size, (255, 255, 255, 0))
        white.putalpha(alpha)
        white.save(art / name)
        print(f"{art / name}: {alpha.width} x {alpha.height}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
