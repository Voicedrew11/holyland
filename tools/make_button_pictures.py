#!/usr/bin/env python3
"""Writes port/gs/shared/button_pictures_data.h: what the scene renderer needs to draw the controller's buttons
again sharper (port/gs/shared/button_pictures.h). Committed: building the port needs no disc, only changing which
buttons are redrawn does.

    python3 tools/make_button_pictures.py --iso path/to/kfiv_us.iso [--preview DIR]

Needs numpy and Pillow. The pictures with buttons (SLUS-20318), both in menu/menu.tmx:
- block 12, the sheet of buttons (128 x 64: cross, circle, square, triangle, the d-pad's arms, L1 L2 R1 R2, each in a
  23 x 23 cell); the first four are redrawn.
- block 1, the sheet of button hints (512 x 512, text_pictures.h's): its cross, circle, square and triangle buttons
  beside the hints, found in it (round, 21 texels across) and told apart by their symbol's colour.
Of each picture only a hash is written (to know it when the game sends it) and, for each redrawn button, where its
cell is and what its symbol is measured to be: the shapes it is made of (two bars, a ring, an outlined square or
triangle), in texels, and its colour. The button's dark round body is not written: the port fits its shading to the
game's own texels each time (button_pictures.cpp), as this tool does to measure the symbol.

--preview DIR also writes each button drawn 16 times as fine (and 4 times, as the port draws it) beside the disc's,
from the same fit, to judge the shapes by.
"""

import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "port", "gs", "shared", "button_pictures_data.h")

TMX = "menu/menu.tmx"
SHEET_BLOCK, HINTS_BLOCK = 12, 1
CELL = 23            # (texels across a button's cell)
CENTRE = 11.5        # (the round body's middle in its cell)
RADIUS = 10.6        # (the body's radius: the disc's texels are those whose middles are inside it)
SUB = 6              # (samples across a texel when a texel's cover is measured)

# The shading model of a button's body, the same as button_pictures.cpp's: rings (Gaussian across the radius)
# times a few waves round it. Where the symbol hides the body the rings have no texels: the fit bends them as little as
# it can there (BEND, on each wave's second difference from ring to ring), and keeps them small (RIDGE)
RING_STEP, RING_SIGMA, WAVES, BEND, RIDGE = 0.75, 0.55, 4, 30.0, 0.01
CENTRE_FADE = 2.0  # (texels from the middle over which the waves fade in)

# Each symbol's shape and its starting guess, fitted from there
SHAPES = {
    "cross": ("cross", [0, -9, 9, 1.9, 0, -9, 9, 1.9]),
    "circle": ("ring", [11.5, 11.5, 7.0, 1.5]),
    "square": ("square", [11.5, 11.0, 6.5, 6.5, 1.5]),
    "triangle": ("triangle", [11.5, 4.5, 11.5, 16.5, 7.0, 1.2]),
}
# The sheet's buttons redrawn: (symbol, column, row)
SHEET_BUTTONS = [("cross", 0, 0), ("circle", 1, 0), ("square", 2, 0), ("triangle", 3, 0)]
# The hints' symbols by their colour's hue (0 to 1 round the colour wheel): (symbol, lowest, highest)
HUES = [("circle", 0.95, 1.0), ("circle", 0.0, 0.12), ("triangle", 0.25, 0.5), ("cross", 0.5, 0.75),
        ("square", 0.75, 0.95)]


# --- the disc ----------------------------------------------------------------------------------------------------

def iso_file(iso, path):
    """the bytes of a file on an ISO 9660 image, by its path ("DATA/KF4.DAT")"""
    def records(extent, size):
        iso.seek(extent * 2048)
        data = iso.read(size)
        at = 0
        while at < len(data):
            length = data[at]
            if length == 0:
                at = (at // 2048 + 1) * 2048
                continue
            rec = data[at:at + length]
            name = rec[33:33 + rec[32]].decode("ascii", "replace").split(";")[0]
            yield name, struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            at += length
    iso.seek(16 * 2048)
    pvd = iso.read(2048)
    extent, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
    for part in path.split("/"):
        for name, e, s in records(extent, size):
            if name.upper() == part.upper():
                extent, size = e, s
                break
        else:
            sys.exit(f"{path}: not on the disc")
    iso.seek(extent * 2048)
    return extent * 2048, size


def read_dat_file(iso_path, name):
    with open(iso_path, "rb") as iso:
        start, _ = iso_file(iso, "DATA/KF4.DAT")
        iso.seek(start)
        head = iso.read(0x40)
        count = struct.unpack_from("<I", head, 4)[0]
        table = iso.read(count * 64)
        for i in range(count):
            entry = table[i * 64:(i + 1) * 64]
            if entry[:52].split(b"\0")[0].decode() == name:
                size, _, offset = struct.unpack_from("<III", entry, 52)
                iso.seek(start + offset)
                return iso.read(size)
    sys.exit(f"{name}: not in KF4.DAT")


def sheet_picture(data, block):
    """(width, height, indices, RGBA colours as the GS reads them, alpha 0-255) of a .tmx block in PSMT8"""
    at = struct.unpack_from("<i", data, 16 + 16 * block)[0]
    image_size = struct.unpack_from("<i", data, at + 4)[0]
    tex0 = struct.unpack_from("<Q", data, at + 16)[0]
    psm, width, height = (tex0 >> 20) & 0x3F, 1 << ((tex0 >> 26) & 15), 1 << ((tex0 >> 30) & 15)
    assert psm == 0x13 and (tex0 >> 51) & 15 == 0, "PSMT8 with a 32-bit CLUT expected"
    indices = np.frombuffer(data, np.uint8, width * height, at + 32).reshape(height, width)
    clut = np.frombuffer(data, np.uint8, 1024, at + 32 + image_size).reshape(256, 4).astype(float)
    i = np.arange(256)
    clut = clut[(i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)]  # (CSM1: the CLUT's rows stored swizzled)
    clut[:, 3] = np.minimum(clut[:, 3] * 2, 255)  # (the GS's alpha 0x80 is opaque)
    return width, height, indices, clut[indices]


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# --- shapes ------------------------------------------------------------------------------------------------------
# A symbol is parts: a ring (centre, outer radius) or a convex polygon, each filled (stroke 0) or outlined inward by
# stroke texels. A point's depth in a part is how far inside its edge it is.

def parts_of(kind, p):
    if kind == "cross":
        r2 = np.sqrt(2)
        def bar(perp, start, end, width, across):
            # (a bar along u (x + y) at v = perp, or along v (x - y) at u = perp; u, v from the cell's middle)
            corners = [(start, perp - width / 2), (end, perp - width / 2), (end, perp + width / 2),
                       (start, perp + width / 2)]
            if across:
                corners = [(b, a) for a, b in corners]
            return ("polygon", 0.0, [(CENTRE + (u + v) / r2, CENTRE + (u - v) / r2) for u, v in corners])
        return [bar(*p[0:4], False), bar(*p[4:8], True)]
    if kind == "ring":
        cx, cy, radius, stroke = p
        return [("ring", stroke, (cx, cy, radius))]
    if kind == "square":
        cx, cy, hx, hy, stroke = p
        return [("polygon", stroke, [(cx - hx, cy - hy), (cx + hx, cy - hy), (cx + hx, cy + hy), (cx - hx, cy + hy)])]
    if kind == "triangle":
        ax, ay, bx, by, half_base, stroke = p
        return [("polygon", stroke, [(ax, ay), (bx + half_base, by), (bx - half_base, by)])]
    raise ValueError(kind)


def depth(part, x, y):
    kind, _, geometry = part
    if kind == "ring":
        cx, cy, radius = geometry
        return radius - np.hypot(x - cx, y - cy)
    corners = geometry
    area = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(corners, corners[1:] + corners[:1]))
    turn = 1 if area > 0 else -1  # (which side of each edge is inside)
    d = np.full(np.broadcast(x, y).shape, np.inf)
    for (x0, y0), (x1, y1) in zip(corners, corners[1:] + corners[:1]):
        length = np.hypot(x1 - x0, y1 - y0)
        d = np.minimum(d, turn * ((x1 - x0) * (y - y0) - (y1 - y0) * (x - x0)) / length)
    return d


def covered(parts, x, y, grow=0.0):
    """whether points are inside the symbol; grow widens every part's ink by that much each side"""
    inside = np.zeros(np.broadcast(x, y).shape, bool)
    for part in parts:
        d = depth(part, x, y)
        stroke = part[1]
        inside |= (d >= -grow) & ((stroke == 0) | (d <= stroke + grow))
    return inside


# --- the fit -----------------------------------------------------------------------------------------------------

def samples(n, sub):
    """(x, y) of sub x sub points in each of n x n texels: arrays (n, n, sub, sub)"""
    o = (np.arange(sub) + 0.5) / sub
    ys, xs = np.meshgrid(np.arange(n), np.arange(n), indexing="ij")
    return np.broadcast_arrays(xs[..., None, None] + o[None, None, None, :], ys[..., None, None] + o[None, None, :, None])


def shading_basis(x, y):
    dx, dy = x - CENTRE, y - CENTRE
    r = np.minimum(np.hypot(dx, dy), RADIUS)
    t = np.arctan2(dy, dx)
    # (each ring but the middle one mirrored through the middle, so it has no point there and even rings sum flat)
    rings = [np.exp(-0.5 * ((r - c) / RING_SIGMA) ** 2) + (c > 0) * np.exp(-0.5 * ((r + c) / RING_SIGMA) ** 2)
             for c in np.arange(0, RADIUS + 1, RING_STEP)]
    fade = np.minimum(r / CENTRE_FADE, 1.0)  # (the waves fade out at the middle, where they meet)
    waves = [np.ones_like(t)] + [fade * f(k * t) for k in range(1, WAVES + 1) for f in (np.cos, np.sin)]
    return np.stack([a * b for a in rings for b in waves], axis=-1)


def smoothing(n):
    """the fit's penalty matrix: BEND on each wave's second difference across the rings, RIDGE on every term"""
    w = 2 * WAVES + 1
    rings = n // w
    d = np.zeros(((rings - 2) * w, n))
    for r in range(1, rings - 1):
        for k in range(w):
            row = (r - 1) * w + k
            d[row, (r - 1) * w + k], d[row, r * w + k], d[row, (r + 1) * w + k] = 1, -2, 1
    return BEND * d.T @ d + RIDGE * np.eye(n)


class Button:
    def __init__(self, colours):
        self.rgb, self.alpha = colours[..., :3], colours[..., 3]
        self.x, self.y = samples(CELL, SUB)
        self.basis = shading_basis(self.x, self.y).mean(axis=(2, 3))
        self.inside = self.alpha >= self.alpha.max() / 2  # (the hints' sheet is not quite clear round them)

    def body(self, parts):
        """the body's shading coefficients (one column per colour), fitted to the texels the symbol leaves alone"""
        keep = self.inside & ~covered(parts, self.x, self.y, grow=0.5).any(axis=(2, 3))
        a = self.basis[keep]
        return np.linalg.solve(a.T @ a + smoothing(a.shape[1]), a.T @ self.rgb[keep])

    def error(self, parts, body):
        """(mean square error, the symbol's colour fitted for it)"""
        cover = covered(parts, self.x, self.y).mean(axis=(2, 3))[self.inside][:, None]
        under = (self.basis @ body)[self.inside]
        seen = self.rgb[self.inside]
        # (seen = under (1 - cover) + colour cover: the colour by least squares)
        colour = ((seen - under * (1 - cover)) * cover).sum(axis=0) / max((cover ** 2).sum(), 1e-9)
        colour = np.clip(colour, 0, 255)
        return np.mean((under * (1 - cover) + colour * cover - seen) ** 2), colour

    def fit(self, kind, guess):
        p = np.array(guess, float)
        for _ in range(3):  # (the body is fitted round the symbol, so the two are fitted in turn)
            body = self.body(parts_of(kind, p))
            e, _ = self.error(parts_of(kind, p), body)
            step = 0.5
            while step > 0.01:
                moved = False
                for i in range(len(p)):
                    for d in (step, -step):
                        q = p.copy()
                        q[i] += d
                        eq, _ = self.error(parts_of(kind, q), body)
                        if eq < e:
                            p, e, moved = q, eq, True
                if not moved:
                    step /= 2
        body = self.body(parts_of(kind, p))
        e, colour = self.error(parts_of(kind, p), body)
        return p, body, colour, np.sqrt(e)


def drawn(parts, body, colour, scale, sub=4):
    """the button drawn scale times as fine: RGBA 0-255"""
    n = CELL * scale
    x, y = samples(n, sub)
    x, y = x / scale, y / scale
    shade = np.zeros(x.shape + (3,))
    for i in range(0, n, 32):
        shade[i:i + 32] = shading_basis(x[i:i + 32], y[i:i + 32]) @ body
    ink = covered(parts, x, y)[..., None]
    rgb = np.where(ink, colour, shade)
    disc = (np.hypot(x - CENTRE, y - CENTRE) <= RADIUS).astype(float)
    a = disc.mean(axis=(2, 3))
    c = (rgb * disc[..., None]).sum(axis=(2, 3)) / np.maximum(disc.sum(axis=(2, 3)), 1e-9)[..., None]
    return np.dstack([np.clip(c, 0, 255), a * 255]).round().astype(np.uint8)


# --- the header --------------------------------------------------------------------------------------------------

def write_header(pictures):
    """pictures: [(name, width, height, hash, [(symbol, cell x, cell y, parts, colour)])]"""
    lines = [
        "// Generated by tools/make_button_pictures.py from the disc's pictures of buttons: do not edit. What",
        "// port/gs/shared/button_pictures.h draws: only each picture's hash and its buttons' measured symbols are here.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace pcr::button_pictures_data {",
        "",
        f"constexpr uint32_t cell = {CELL}; // (texels across a button's cell)",
        f"constexpr float centre = {CENTRE}f, radius = {RADIUS}f; // (the round body in its cell, texels)",
        "// (the body's shading model: tools/make_button_pictures.py)",
        f"constexpr float ring_step = {RING_STEP}f, ring_sigma = {RING_SIGMA}f, bend = {BEND}f, ridge = {RIDGE}f;",
        f"constexpr uint32_t rings = {len(np.arange(0, RADIUS + 1, RING_STEP))}, waves = {WAVES};",
        f"constexpr float centre_fade = {CENTRE_FADE}f;",
        "",
        "enum class PartKind { Ring, Polygon };",
        "",
        "// A part of a symbol: a ring (x[0], y[0] its centre, x[1] its outer radius) or a convex polygon of corners",
        "// corners; filled when stroke is 0, else outlined inward stroke texels wide. Texels from the cell's corner.",
        "struct Part {",
        "    PartKind kind;",
        "    float stroke;",
        "    uint32_t corners;",
        "    float x[4], y[4];",
        "};",
        "",
        "struct Button {",
        "    const char *symbol;",
        "    uint16_t cell_x, cell_y; // (its cell's corner in its picture)",
        "    uint8_t r, g, b;         // (its symbol's colour)",
        "    uint32_t first_part, part_count;",
        "};",
        "",
        "struct Picture {",
        "    const char *name;",
        "    uint16_t width, height;",
        "    uint64_t hash; // (FNV-1a of its indices, rows top down)",
        "    uint32_t first_button, button_count;",
        "};",
        "",
        "static const Part parts[] = {",
    ]
    f = lambda v: ", ".join(f"{a:.3f}f" for a in v)
    button_lines, picture_lines = [], []
    part_count = button_count = 0
    for name, width, height, hash_, buttons in pictures:
        picture_lines.append(f'    {{"{name}", {width}, {height}, 0x{hash_:016x}ull, {button_count}, {len(buttons)}}},')
        for symbol, x, y, parts, colour in buttons:
            r, g, b = (int(round(v)) for v in colour)
            button_lines.append(f'    {{"{symbol}", {x}, {y}, {r}, {g}, {b}, {part_count}, {len(parts)}}}, // {name}')
            button_count += 1
            for kind, stroke, geometry in parts:
                if kind == "ring":
                    cx, cy, radius = geometry
                    xs, ys, n = [cx, radius, 0, 0], [cy, 0, 0, 0], 0
                else:
                    xs = [c[0] for c in geometry] + [0] * (4 - len(geometry))
                    ys = [c[1] for c in geometry] + [0] * (4 - len(geometry))
                    n = len(geometry)
                lines.append(f"    {{PartKind::{'Ring' if kind == 'ring' else 'Polygon'}, {stroke:.3f}f, {n}, "
                             f"{{{f(xs)}}}, {{{f(ys)}}}}}, // {name} {symbol} at {x} {y}")
                part_count += 1
    lines += ["};", "", "static const Button buttons[] = {"] + button_lines
    lines += ["};", "", "static const Picture pictures[] = {"] + picture_lines
    lines += ["};", "", "} // namespace pcr::button_pictures_data", ""]
    with open(OUT, "w") as out:
        out.write("\n".join(lines))


def hint_buttons(colours):
    """[(symbol, cell x, cell y)] of the round buttons on the hints' sheet: opaque discs 21 texels across with a
    coloured symbol, told apart by its hue"""
    import colorsys
    opaque = colours[..., 3] >= 128
    height, width = opaque.shape
    seen = np.zeros_like(opaque)
    found = []
    for y0 in range(height):
        for x0 in range(width):
            if not opaque[y0, x0] or seen[y0, x0]:
                continue
            stack, points = [(x0, y0)], []
            seen[y0, x0] = True
            while stack:
                x, y = stack.pop()
                points.append((x, y))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < width and 0 <= ny < height and opaque[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        stack.append((nx, ny))
            xs, ys = [p[0] for p in points], [p[1] for p in points]
            # (a button's disc is 21 texels across, one of the hints' 22 down)
            if max(xs) - min(xs) not in (20, 21) or max(ys) - min(ys) not in (20, 21) or not 300 < len(points) < 380:
                continue
            cx, cy = min(xs) - 1, min(ys) - 1
            cell = colours[cy:cy + CELL, cx:cx + CELL, :3]
            saturation = cell.max(axis=2) - cell.min(axis=2)
            if saturation.max() < 40:
                continue
            r, g, b = cell[saturation >= saturation.max() * 0.8].mean(axis=0) / 255
            hue = colorsys.rgb_to_hsv(r, g, b)[0]
            symbol = next(s for s, lo, hi in HUES if lo <= hue <= hi)
            found.append((symbol, cx, cy))
    return sorted(found, key=lambda f: (f[2], f[1]))


def preview(folder, label, cell, parts, body, colour):
    os.makedirs(folder, exist_ok=True)
    old = Image.fromarray(cell.round().astype(np.uint8), "RGBA").resize((CELL * 16,) * 2, Image.NEAREST)
    new = Image.fromarray(drawn(parts, body, colour, 16), "RGBA")
    Image.fromarray(drawn(parts, body, colour, 4), "RGBA").save(os.path.join(folder, f"{label}_x4.png"))
    new.save(os.path.join(folder, f"{label}_x16.png"))
    side = Image.new("RGBA", (CELL * 32 + 30, CELL * 16 + 20), (80, 80, 80, 255))
    side.alpha_composite(old, (10, 10))
    side.alpha_composite(new, (CELL * 16 + 20, 10))
    side.save(os.path.join(folder, f"{label}_compare.png"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--preview", help="a folder to write each button drawn finer, beside the disc's")
    args = parser.parse_args()

    tmx = read_dat_file(args.iso, TMX)
    pictures = []
    for name, block in (("sheet", SHEET_BLOCK), ("hints", HINTS_BLOCK)):
        width, height, indices, colours = sheet_picture(tmx, block)
        if block == SHEET_BLOCK:
            places = [(symbol, column * CELL, row * CELL) for symbol, column, row in SHEET_BUTTONS]
        else:
            places = hint_buttons(colours)
        buttons = []
        for symbol, x, y in places:
            cell = colours[y:y + CELL, x:x + CELL]
            kind, guess = SHAPES[symbol]
            p, body, colour, rms = Button(cell).fit(kind, guess)
            parts = parts_of(kind, p)
            buttons.append((symbol, x, y, parts, colour))
            print(f"{name} {symbol} at {x} {y}: {kind} {' '.join(f'{v:.2f}' for v in p)}  "
                  f"colour {colour.round().astype(int).tolist()}  rms {rms:.1f}")
            if args.preview:
                preview(args.preview, f"{name}_{symbol}_{x}_{y}", cell, parts, body, colour)
        pictures.append((name, width, height, fnv1a(indices.tobytes()), buttons))
    write_header(pictures)
    print(f"wrote {OUT} ({sum(len(p[4]) for p in pictures)} buttons)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
