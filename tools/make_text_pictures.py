#!/usr/bin/env python3
"""Writes port/gs/shared/text_pictures_data.h: what the scene renderer needs to draw the game's word pictures again
in IPA P Mincho (port/gs/shared/text_pictures.h). Committed: building the port needs neither the font nor the disc,
only changing the words does.

    python3 tools/make_text_pictures.py --iso path/to/kfiv_us.iso [--font port/ui/res/fonts/HolylandMincho.ttf]

Needs Pillow and pycdlib (pip install pillow pycdlib). The disc gives each picture's texels, of which only a hash is
written (to know the picture when the game sends it); the font gives the letters, written as pictures of text, which
the IPA Font License (third_party/ipa-fonts/) lets us ship.

The pictures (SLUS-20318):
- menu/menu.tmx, the menu's: its 21 buttons (256 x 64, a word on a blue glow) and the sheet of button hints
  (block 1, 512 x 512: ":Back", ":Confirm" and so on beside the controller's buttons, on nothing).
The title's and game over's choices (title/title_menu1-5.tm2, title/gameover_*.tm2) are left as the disc has them:
their typewriter letters are not the Mincho the rest is redrawn in.
Each word is given by the part of its picture it is in (a region, in texels) and the rows its capitals stand on;
where the old word's ink lies inside the region is found by the port each time, from the picture.
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "port", "gs", "shared", "text_pictures_data.h")
FONT = os.path.join(ROOT, "port", "ui", "res", "fonts", "HolylandMincho.ttf")

SCALE = 4  # (text_picture_scale: the finer picture's texels across one of the game's)
CAP = 20 * SCALE  # (the letters' capitals' height, in finer texels: the menu buttons', drawn as they are)
SUPERSAMPLE = 4  # (each letter drawn this much finer, then averaged: smooth edges)
STROKE = 2  # (supersampled pixels added round each stroke: IPA P Mincho's hairlines a little firmer)

# (name in KF4.DAT, block of a .tmx or None for a lone picture, background, [(word, region x0 y0 x1 y1, cap top,
# baseline, alignment)]); rows: the capitals' top row and the row under their foot
BUTTON = (0, 10, 256, 56)
PICTURES = []
for block, word in [(14, "Load"), (15, "Quit"), (16, "Load Data"), (17, "Settings"), (18, "Key Config"), (19, "Buy"),
                    (20, "Upgrade"), (21, "Repair"), (22, "Sell"), (23, "Receive"), (24, "Item"), (25, "Save Data"),
                    (26, "Brightness"), (27, "Magic"), (28, "Equipment"), (29, "Status"), (30, "Keys"),
                    (31, "Journal"), (32, "Arrange"), (33, "System"), (38, "Exchange")]:
    PICTURES.append(("menu/menu.tmx", block, "GLOW", [(word, BUTTON, 22, 42, "CENTRE")]))


def hint(word, column, row):
    x0, x1 = ((88, 190), (215, 322), (343, 440))[column]  # (from each colon to before the next column's buttons)
    baseline = 88 + 32 * row
    return (word, (x0, baseline - 18, x1, baseline + 5), baseline - 13, baseline, "LEFT")


PICTURES.append(("menu/menu.tmx", 1, "PLAIN", [
    hint(":Back", 0, 0), hint(":Back", 0, 1), hint(":Status", 0, 2), hint(":Low", 0, 3), hint(":List", 0, 4),
    hint(":Hide/Show", 1, 0), hint(":Select", 1, 1), hint(":Confirm", 1, 2), hint(":High", 1, 3),
    hint(":Next Page", 1, 4),
    hint(":Back", 2, 0), hint(":Keys", 2, 1), hint(":Items", 2, 2),
]))


def read_dat(iso_path, names):
    import pycdlib
    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    files = {}
    with iso.open_file_from_iso(iso_path="/DATA/KF4.DAT;1") as dat:
        count = struct.unpack_from("<I", dat.read(0x40), 4)[0]
        table = dat.read(count * 64)
        for i in range(count):
            entry = table[i * 64:(i + 1) * 64]
            name = entry[:52].split(b"\0")[0].decode()
            size, _, offset = struct.unpack_from("<III", entry, 52)
            if name in names:
                dat.seek(offset)
                files[name] = dat.read(size)
    iso.close()
    return files


def picture(data, block):
    """(width, height, indices) of a PSMT8 picture: a lone one or a .tmx's block"""
    at = 0 if block is None else struct.unpack_from("<i", data, 16 + 16 * block)[0]
    tex0 = struct.unpack_from("<Q", data, at + 16)[0]
    psm, width, height = (tex0 >> 20) & 0x3F, 1 << ((tex0 >> 26) & 15), 1 << ((tex0 >> 30) & 15)
    assert psm == 0x13, f"block {block}: PSMT8 expected, {psm:#x}"
    indices = data[at + 32:at + 32 + width * height]
    return width, height, indices


def fnv1a(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def letters(font_path, characters):
    """{character: (left, top, width, height, advance, coverage bytes)}, in finer texels from the pen on the
    baseline; capitals CAP tall"""
    probe = ImageFont.truetype(font_path, 1000)
    cap = -probe.getbbox("H", anchor="ls")[1]
    font = ImageFont.truetype(font_path, round(1000 * CAP * SUPERSAMPLE / cap))
    out = {}
    pad = 4 * SUPERSAMPLE
    for ch in sorted(characters):
        if ch == " ":
            out[ch] = (0, 0, 0, 0, round(font.getlength(ch) / SUPERSAMPLE), b"")
            continue
        l, t, r, b = font.getbbox(ch, anchor="ls", stroke_width=STROKE)
        # (drawn with the pen at a whole finer texel, so the supersampled box lands on finer texels)
        x0 = (l - pad) // SUPERSAMPLE * SUPERSAMPLE
        y0 = (t - pad) // SUPERSAMPLE * SUPERSAMPLE
        x1 = -(-(r + pad) // SUPERSAMPLE) * SUPERSAMPLE
        y1 = -(-(b + pad) // SUPERSAMPLE) * SUPERSAMPLE
        im = Image.new("L", (x1 - x0, y1 - y0), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor="ls", stroke_width=STROKE,
                                stroke_fill=255)
        im = im.resize((im.width // SUPERSAMPLE, im.height // SUPERSAMPLE), Image.BOX)
        box = im.getbbox()
        im = im.crop(box)
        left = x0 // SUPERSAMPLE + box[0]
        top = -(y0 // SUPERSAMPLE + box[1])  # (rows above the baseline)
        out[ch] = (left, top, im.width, im.height, round(font.getlength(ch) / SUPERSAMPLE), im.tobytes())
    return out


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_header(glyphs, pictures):
    lines = [
        "// Generated by tools/make_text_pictures.py from IPA P Mincho (third_party/ipa-fonts/) and the disc's word",
        "// pictures' hashes: do not edit. What port/gs/shared/text_pictures.h draws.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace pcr::text_pictures_data {",
        "",
        f"constexpr uint32_t scale = {SCALE};",
        f"constexpr uint32_t cap_height = {CAP}; // (finer texels)",
        "",
        "// A letter: its picture's place from the pen on the baseline (left; top, rows above it), in finer texels",
        "struct Glyph {",
        "    char character;",
        "    int16_t left, top;",
        "    uint16_t width, height, advance;",
        "    uint32_t start; // (in coverage)",
        "};",
        "",
        "enum class Background { Glow, Plain };",
        "enum class Align { Left, Centre };",
        "",
        "struct Word {",
        "    const char *text;",
        "    uint16_t x0, y0, x1, y1;    // (the region it is in, texels)",
        "    uint16_t cap_top, baseline; // (rows)",
        "    Align align;",
        "};",
        "",
        "struct Picture {",
        "    uint16_t width, height;",
        "    uint64_t hash; // (FNV-1a of its indices, rows top down)",
        "    Background background;",
        "    uint32_t first_word, word_count; // (in words)",
        "};",
        "",
    ]
    lines.append("static const Glyph glyphs[] = {")
    start = 0
    for ch, (left, top, w, h, adv, _) in glyphs.items():
        lines.append(f"    {{{ord(ch)}, {left}, {top}, {w}, {h}, {adv}, {start}}}, // {ch!r}")
        start += w * h
    lines.append("};")
    lines.append("")
    lines.append("static const Word words[] = {")
    for p in pictures:
        for (word, (x0, y0, x1, y1), cap_top, baseline, align) in p["words"]:
            a = "Align::Left" if align == "LEFT" else "Align::Centre"
            lines.append(f"    {{{c_string(word)}, {x0}, {y0}, {x1}, {y1}, {cap_top}, {baseline}, {a}}},")
    lines.append("};")
    lines.append("")
    lines.append("static const Picture pictures[] = {")
    first = 0
    for p in pictures:
        bg = "Background::Glow" if p["background"] == "GLOW" else "Background::Plain"
        lines.append(f"    {{{p['width']}, {p['height']}, {p['hash']:#018x}ull, {bg}, {first}, {len(p['words'])}}}, "
                     f"// {p['name']}" + (f" block {p['block']}" if p["block"] is not None else ""))
        first += len(p["words"])
    lines.append("};")
    lines.append("")
    lines.append(f"// each letter's coverage, 0 (none) to 255, its rows top down ({start} bytes)")
    lines.append("static const uint8_t coverage[] = {")
    for ch, (_, _, w, h, _, data) in glyphs.items():
        if not data:
            continue
        lines.append(f"    // {ch!r}")
        for row in range(h):
            lines.append("    " + ",".join(str(b) for b in data[row * w:(row + 1) * w]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("} // namespace pcr::text_pictures_data")
    lines.append("")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--iso", required=True)
    parser.add_argument("--font", default=FONT)
    args = parser.parse_args()

    files = read_dat(args.iso, {p[0] for p in PICTURES})
    pictures = []
    characters = set()
    for name, block, background, words in PICTURES:
        width, height, indices = picture(files[name], block)
        pictures.append({"name": name, "block": block, "background": background, "words": words, "width": width,
                         "height": height, "hash": fnv1a(indices)})
        for w in words:
            characters.update(w[0])
    hashes = [(p["width"], p["height"], p["hash"]) for p in pictures]
    assert len(set(hashes)) == len(hashes), "two pictures with the same hash"
    glyphs = letters(args.font, characters)
    write_header(glyphs, pictures)
    print(f"wrote {OUT} ({len(pictures)} pictures, {len(glyphs)} letters, "
          f"{sum(g[2] * g[3] for g in glyphs.values())} bytes of coverage)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
