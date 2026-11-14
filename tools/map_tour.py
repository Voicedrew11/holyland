#!/usr/bin/env python3
"""A tour of the game's maps: every map visited and pictured, without playing there.

    tools/map_tour.py --out DIR [--disc ISO] [--exe PATH] [--maps 0,99,...] [--positions N]
                      [--headings N] [--jobs N] [--base FILE] [--no-capture] [--renderer gs|pc]
                      [--extra "ARGS"] [--turn] [--keep-lists DIR]

A quick save is the game's own save image with the map and the player's place in it, loaded through the
game's card-load path (docs/port/README.md, "Quick saves"). This makes one for any map and any place:
it takes a real save image once (tests/newgame.pad, --quick-save-at), then writes copies of it with the
map tag, the position and the heading changed (and the image's byte sums and the file's hash made
again), marked "made up" (the PC block's flags bit 1): loading such a file puts the player on the
floor under the position (Collision_FindFloorBelow, game/quicksave.c) and works out the neighbouring
map from there.

The places come from the map file itself (read from KF4.DAT on the disc): the map's own start
(MapSlotUnk08.player_start_*, where a map change without a link puts the player), then the middles of
the biggest boxes of the records that have floors to stand on. Each place is seen at --headings
headings.

Each map is one run of the program: headless with the GS on (the PC renderer, the default here;
--renderer gs for paraLLEl-GS's pictures), it loads the map's quick saves one after another
(--quick-load A,V:B,...), writes the picture of each view once the fade-in is over, and a GS frame capture of the first view
(--gs-capture-at). Then kf4-gsreplay lists the capture's draws (the
census: VU1 programs, PRIM, TEX0 PSM, ALPHA, TEST, ZBUF, FRAME) and runs its VIF1 stream through the
VU1 interpreter again (--mode vif: VU1 runs and cycles for the frame, and packets that come out
differently).

Into --out it writes, per map, NNN/sheet.png (the views, labelled), NNN/log.txt, NNN/stderr.txt and
NNN/capture.kf4gs, and over all maps report.html, report.md and report.json. The saves, pictures and
captures hold the game's data: keep them out of the repository (--out outside it).
"""
import argparse
import concurrent.futures
import html
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NEWGAME_PAD = os.path.join(ROOT, "tests", "newgame.pad")
GAME_DATA_DOC = os.path.join(ROOT, "decomp", "docs", "GAME_DATA.md")

# ---- The save image (Game_WriteSaveImage, core/game.c) ------------------------------------------
QUICK_HEADER_SIZE = 40
QUICK_INFO_SIZE = 68
IMAGE_MAGIC = 0x15400
IMAGE_PLAYER = 0x13FB0      # PlayerState 0x00: the position (x, y, z, w), then 0x10 the rotation
IMAGE_MAP_TAG = 0x14018     # the map tag (Game_ReadSaveImage: saved_map_tag)
FLAG_MADE_UP = 2            # the PC block's flags byte (port/platform/quicksave.c)

# ---- The map file (map/map.h: MapSlotUnk08, MapRecordBounds) ------------------------------------
MAP_RECORDS = 0x17C0
MAP_RECORD_SIZE = 0x80
RECORD_NO_COLLISION = 0x40000000

# ---- The run --------------------------------------------------------------------------------------
FIRST_LOAD = 130    # the first view is loaded on the title screen (about vblank 123, after the card notice)
STRIDE = 100        # vblanks per view: the load, the fade in (about 70 vblanks), the picture
SETTLE = 90         # the picture, this long after the view's load was asked for
CLOCK = "2026-01-01T12:00:00"


def find_exe(name):
    for preset in ("linux-clang", "linux-gcc", "windows-clang"):
        for suffix in ("", ".exe"):
            path = os.path.join(ROOT, "out", preset, name + suffix)
            if os.path.exists(path):
                return path
    return None


# ---- The disc: ISO 9660 or a folder, then KF4.DAT ----------------------------------------------
class Disc:
    def __init__(self, path):
        self.path = path
        if os.path.isdir(path):
            self.file = open(os.path.join(path, "DATA", "KF4.DAT"), "rb")
            self.base = 0
        else:
            self.file = open(path, "rb")
            self.base = self._find_iso("DATA/KF4.DAT")
        self.file.seek(self.base)
        head = self.file.read(0x40)
        (count,) = struct.unpack_from("<I", head, 4)
        table = self.file.read(count * 64)
        self.files = {}
        for i in range(count):
            entry = table[i * 64:(i + 1) * 64]
            name = entry[:52].split(b"\0")[0].decode("ascii")
            size, _padded, offset = struct.unpack_from("<III", entry, 52)
            self.files[name] = (offset, size)

    def _sectors(self, lba, count):
        self.file.seek(lba * 2048)
        return self.file.read(count * 2048)

    def _find_iso(self, path):
        root = self._sectors(16, 1)[156:156 + 34]
        lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
        for part in path.split("/"):
            data = self._sectors(lba, (size + 2047) // 2048)
            i, found = 0, None
            while i < len(data):
                length = data[i]
                if length == 0:
                    i = (i // 2048 + 1) * 2048
                    continue
                entry = data[i:i + length]
                name = entry[33:33 + entry[32]].decode("ascii", "replace").split(";")[0]
                if name.upper() == part.upper():
                    found = struct.unpack_from("<I", entry, 2)[0], struct.unpack_from("<I", entry, 10)[0]
                    break
                i += length
            if found is None:
                sys.exit("map_tour: no %s on %s" % (path, self.path))
            lba, size = found
        return lba * 2048

    def read(self, name):
        offset, size = self.files[name]
        self.file.seek(self.base + offset)
        return self.file.read(size)

    def maps(self):
        return sorted(int(n[:3]) for n in self.files if re.fullmatch(r"\d{3}\.map", n))


def mesh_triangles(data, at, lists):
    """The triangles of some of the strip lists of the collision mesh at `at` (CollisionMesh, as
    Collision_RelocateMesh reads it: a 0x50-byte head, the strips, then the points; the lists are
    offsets from the mesh, and a strip point names a point by number), in the mesh's own space."""
    (strip_count,) = struct.unpack_from("<i", data, at)
    (point_count,) = struct.unpack_from("<I", data, at + 0x0C)
    offsets = struct.unpack_from("<6i", data, at + 0x18)
    counts = struct.unpack_from("<6H", data, at + 0x38)
    cursor = at + 0x50
    for _ in range(strip_count):
        cursor += 0x20 + struct.unpack_from("<H", data, cursor)[0] * 16
    points = cursor
    result = []
    for which in lists:
        triangles = []
        cursor = at + offsets[which]
        for _ in range(counts[which]):
            (n,) = struct.unpack_from("<H", data, cursor)
            corners = [struct.unpack_from("<3f", data, points + 16 * struct.unpack_from("<i", data, cursor + 0x20 + 16 * k)[0])
                       for k in range(n)]
            triangles += [corners[k:k + 3] for k in range(n - 2)]
            cursor += 0x20 + n * 16
        result.append(triangles)
    return result, point_count


def map_floors(data):
    """The floor and hazard triangles of the map's records, in the map's space. Only records that are
    not turned (a scale and a move: their mesh then fits their box, which is checked)."""
    records = struct.unpack_from("<H", data, 0x3C)[0]
    (meshes_at,) = struct.unpack_from("<i", data, 0x08)
    floors, hazards, starts = [], [], [meshes_at]
    for r in range(records):
        at = MAP_RECORDS + r * MAP_RECORD_SIZE
        position = struct.unpack_from("<3f", data, at)
        turn = struct.unpack_from("<3f", data, at + 0x10)
        scale = [s or 1.0 for s in struct.unpack_from("<3f", data, at + 0x20)]
        (mesh,) = struct.unpack_from("<H", data, at + 0x32)
        (flags,) = struct.unpack_from("<I", data, at + 0x3C)
        low = struct.unpack_from("<3f", data, at + 0x60)
        high = struct.unpack_from("<3f", data, at + 0x70)
        if mesh == 0xFFFF or flags & RECORD_NO_COLLISION or any(abs(t) > 1e-4 for t in turn):
            continue
        while len(starts) <= mesh:  # the meshes are back to back (Map_BuildModels): each gives its size
            starts.append(starts[-1] + struct.unpack_from("<i", data, starts[-1] + 0x14)[0])
        (floor, hazard), _ = mesh_triangles(data, starts[mesh], (1, 4))

        def place(triangle):
            return [tuple(p[i] * scale[i] + position[i] for i in range(3)) for p in triangle]

        floor, hazard = [place(t) for t in floor], [place(t) for t in hazard]
        corners = [p for t in floor + hazard for p in t]
        if corners and any(min(p[i] for p in corners) < low[i] - 2 or max(p[i] for p in corners) > high[i] + 2
                           for i in range(3)):
            continue  # not where its box says: read otherwise than thought, so left out
        floors += [(t, r) for t in floor]
        hazards += hazard
    return floors, hazards


def height_at(triangle, x, z):
    """The triangle's y over (x, z), or None when (x, z) is not under it."""
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = triangle
    det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
    if abs(det) < 1e-6:
        return None
    u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / det
    v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / det
    if u < -1e-4 or v < -1e-4 or u + v > 1 + 1e-4:
        return None
    return u * ay + v * by + (1 - u - v) * cy


def map_places(data, count):
    """Places to stand, in the order they are taken: the map's start; the map's fixed items (MapItemDef:
    they lie where the player picks them up) that lie on a record's floor; then the middles of the
    records' biggest floor triangles. None on a hazard (CollisionMesh.hazard_strips: lava, poison), and
    each 1500 or more from those taken before. The game puts the player on the floor under it. (Not the
    entity spawns: a player put on one dies at once.)"""
    start = struct.unpack_from("<4f", data, 0x60)
    places = [(start[0], start[1], start[2], start[3], "start")]
    records, flag_blocks = struct.unpack_from("<HH", data, 0x3C)
    objects, spawns, items = struct.unpack_from("<HHH", data, 0x40)
    floors, hazards = map_floors(data)

    def take(x, floor_y, z, where):
        if len(places) >= count or any(math.hypot(x - p[0], z - p[2]) < 1500 for p in places):
            return
        for triangle in hazards:
            y = height_at(triangle, x, z)
            if y is not None and abs(y - floor_y) < 300:
                return
        places.append((x, floor_y - 50.0, z, 0.0, where))  # y grows downward: from a little over it

    # Map_ParseFile: the records, the flag blocks, the 32 origins, the object placements, the spawns, the items
    item_table = MAP_RECORDS + records * MAP_RECORD_SIZE + flag_blocks * 0x40 + 0x200 + objects * 0x80 + spawns * 0x60
    for i in range(items):
        x, y, z = struct.unpack_from("<3f", data, item_table + i * 0x40)
        under = [h for h in (height_at(t, x, z) for t, _r in floors) if h is not None and y - 50 <= h <= y + 400]
        if under:
            take(x, min(under), z, "item %d" % i)

    def area(triangle):
        (ax, _, az), (bx, _, bz), (cx, _, cz) = triangle
        return abs((bx - ax) * (cz - az) - (cx - ax) * (bz - az)) / 2

    for triangle, r in sorted(floors, key=lambda f: -area(f[0])):
        if area(triangle) < 200 * 200:
            break
        x, y, z = (sum(p[i] for p in triangle) / 3 for i in range(3))
        take(x, y, z, "record %d" % r)
    return places


def map_names():
    """Map number -> the name decomp/docs/GAME_DATA.md gives it (Prima's), if it is there."""
    names = {}
    try:
        with open(GAME_DATA_DOC, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\| ([\d, ]+) \| [\d, ]+ \| ([^|]+) \|", line)
                if m:
                    for n in m.group(1).split(","):
                        names[int(n)] = m.group(2).strip()
    except OSError:
        pass
    return names


# ---- Quick save files (port/platform/quicksave.c) ------------------------------------------------
def fnv1a(data, h=0xCBF29CE484222325):
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_base(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"KF4QUICK":
        sys.exit("map_tour: %s is not a quick save" % path)
    (info_size,) = struct.unpack_from("<I", data, 24)
    image = bytearray(data[QUICK_HEADER_SIZE + info_size:])
    if struct.unpack_from("<I", image, 0)[0] != IMAGE_MAGIC:
        sys.exit("map_tour: %s holds no save image" % path)
    return data[:QUICK_HEADER_SIZE], data[QUICK_HEADER_SIZE:QUICK_HEADER_SIZE + 64], image


def write_made_up(base, path, tag, x, y, z, heading):
    header, info64, image = base
    image = bytearray(image)
    (size,) = struct.unpack_from("<I", image, 4)
    struct.pack_into("<3f", image, IMAGE_PLAYER, x, y, z)
    struct.pack_into("<3f", image, IMAGE_PLAYER + 0x10, 0.0, heading, 0.0)
    struct.pack_into("<i", image, IMAGE_MAP_TAG, tag)
    for block in range(size // 128):  # Game_CheckSaveImage: a byte sum per 128 bytes
        image[size + block] = sum(image[block * 128:(block + 1) * 128]) & 0xFF
    info = bytearray(info64) + bytes([0xFF, 0xFF, 0xFF, FLAG_MADE_UP])
    struct.pack_into("<i3f3f", info, 8, tag, x, y, z, 0.0, heading, 0.0)
    header = bytearray(header)
    struct.pack_into("<I", header, 24, QUICK_INFO_SIZE)
    struct.pack_into("<Q", header, 32, fnv1a(bytes(image), fnv1a(bytes(info))))
    with open(path, "wb") as f:
        f.write(bytes(header) + bytes(info) + bytes(image))


def make_base(exe, disc, folder, given):
    """A real save image: a new game (tests/newgame.pad), saved once the player is in control in the
    first map; or the one given (then the new game is played only as far as the first map, for its
    [todo]s). Returns it and the run's output."""
    path = given or os.path.join(folder, "base.kf4quick")
    cmd = [exe, "--disc", disc, "--no-audio", "--fast", "--headless", "--frames", "1950", "--clock", CLOCK,
           "--memcard", "none", "--input", NEWGAME_PAD, "--log", os.path.join(folder, "base_log.txt")]
    if not given:
        cmd += ["--quick-save-at", "1900:" + path]
    env = dict(os.environ, KF4_DATA_DIR=os.path.join(folder, "base_data"))
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            env=env)
    shutil.rmtree(os.path.join(folder, "base_data"), ignore_errors=True)
    if result.returncode != 0 or not os.path.exists(path):
        sys.stdout.write(result.stdout)
        sys.exit("map_tour: could not take the base quick save")
    return path, result.stdout


# ---- One map ------------------------------------------------------------------------------------
LOG_LINE = re.compile(r"^(\d+) frame=(\d+) menu=(\d+) save=(\d+) status=([0-9A-Fa-f]+) map=(\d):(\w+)(\(empty\))? "
                      r"pos=(\S+),(\S+),(\S+) heading=(\S+) hp=(-?\d+) death=(\d+)")
EVENT = re.compile(r"^(\d+) event: (.*)$")


def parse_log(path):
    states, events = {}, []
    with open(path, errors="replace") as f:
        for line in f:
            m = LOG_LINE.match(line)
            if m:
                states[int(m.group(1))] = {
                    "frame": int(m.group(2)), "status": m.group(5), "map": m.group(7), "empty": bool(m.group(8)),
                    "pos": [float(m.group(9)), float(m.group(10)), float(m.group(11))],
                    "heading": float(m.group(12)), "hp": int(m.group(13)), "death": int(m.group(14))}
                continue
            m = EVENT.match(line)
            if m:
                events.append((int(m.group(1)), m.group(2)))
    return states, events


def state_at(states, vblank):
    for v in range(vblank, vblank - 30, -1):
        if v in states:
            return states[v]
    return None


def picture_stats(path):
    from PIL import Image, ImageStat
    im = Image.open(path).convert("RGB")
    small = im.resize((160, 112))
    grey = small.convert("L")
    mean = ImageStat.Stat(grey).mean[0]
    counts = small.getcolors(160 * 112)
    colours = len(counts)
    # the share of the picture that is one flat colour (the clear colour, a void)
    flat = max(n for n, _colour in counts) / (160 * 112)
    return {"mean": round(mean, 1), "colours": colours, "flat": round(flat, 3)}


def contact_sheet(views, path, title):
    from PIL import Image, ImageDraw
    w, h, pad, top = 320, 224, 4, 18
    cols = 4 if len(views) > 4 else len(views)
    rows = (len(views) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + pad) + pad, top + rows * (h + 14 + pad) + pad), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    draw.text((pad, 3), title, fill=(255, 255, 255))
    for i, view in enumerate(views):
        x, y = pad + (i % cols) * (w + pad), top + (i // cols) * (h + 14 + pad)
        if view.get("picture") and os.path.exists(view["picture"]):
            sheet.paste(Image.open(view["picture"]).convert("RGB").resize((w, h)), (x, y))
        label = "%d: %s h%.2f" % (i, view["place"], view["heading"])
        if view.get("flags"):
            label += " [" + ",".join(view["flags"]) + "]"
        draw.text((x, y + h + 1), label, fill=(255, 255, 160) if view.get("flags") else (200, 200, 200))
    sheet.save(path)


PSM_NAMES = {0x00: "CT32", 0x01: "CT24", 0x02: "CT16", 0x0A: "CT16S", 0x13: "T8", 0x14: "T4", 0x1B: "T8H",
             0x24: "T4HL", 0x2C: "T4HH", 0x30: "Z32", 0x31: "Z24", 0x32: "Z16", 0x3A: "Z16S"}
LIST_LINE = re.compile(r"^\s*(\d+) rec\s+\d+ p(\d) (?:prog (\w+) kick\s+\d+ )?\s*(\w+)((?: tex| abe| fog)*) ctx(\d) "
                       r"v\s*(\d+) k\s*(\d+) box\s*(-?\d+),\s*(-?\d+)-\s*(-?\d+),\s*(-?\d+) z (\w+)-(\w+) \| FRAME fbp (\d+) "
                       r"fbw (\d+) psm (\w+) fbmsk (\w+) \| ZBUF zbp (\d+) psm (\w+) zmsk (\d) \| TEST ate (\d) atst (\d) "
                       r"aref (\w+) afail (\d) date (\d) datm (\d) zte (\d) ztst (\d) \| ALPHA (\d+) fix (\w+) fba (\d) "
                       r"pabe (\d) \| TEX0 (\w+) TEX1 (\w+)")


def census(lines):
    """The draws of a capture, by VU1 program and by GS state combination."""
    programs, combos, draws, kicks, odd = {}, {}, 0, 0, []
    for line in lines:
        m = LIST_LINE.match(line)
        if not m:
            continue
        draws += 1
        kicks += int(m.group(8))
        path, prog, prim, flags = m.group(2), m.group(3), m.group(4), m.group(5).split()
        program = "p%s %s" % (path, prog) if prog else "p%s" % path
        programs[program] = programs.get(program, 0) + 1
        tex0 = int(m.group(34), 16)
        psm = PSM_NAMES.get((tex0 >> 20) & 0x3F, "%02X" % ((tex0 >> 20) & 0x3F)) if "tex" in flags else "-"
        if "tex" in flags and (tex0 >> 20) & 0x3F not in PSM_NAMES:
            odd.append("draw %s: TEX0 %016X (no such PSM), %s vertices, %s kicks" % (m.group(1), tex0, m.group(7),
                                                                                     m.group(8)))
        test = "ate%s/%s zte%s/%s%s" % (m.group(22), m.group(23), m.group(28), m.group(29),
                                        " date" if m.group(26) == "1" else "")
        # ALPHA's ABCD, and FIX only as "fix" where C picks it (its value is a fade, not a state)
        alpha = ("%s%s" % (m.group(30), "/fix" if m.group(30)[2] == "2" else "")) if "abe" in flags else "-"
        key = "%s %s%s psm=%s alpha=%s %s zbuf=%s%s fbp=%s/%s" % (
            program, prim, "".join("+" + f for f in flags), psm, alpha, test, m.group(20),
            " zmsk" if m.group(21) == "1" else "", m.group(15), m.group(17))
        combos[key] = combos.get(key, 0) + 1
    return {"draws": draws, "kicks": kicks, "programs": programs, "combos": combos, "odd": odd}


def run_map(args, base, number, name, places, folder):
    os.makedirs(folder, exist_ok=True)
    saves = os.path.join(folder, "saves")
    pictures = os.path.join(folder, "pictures")
    data_dir = os.path.join(folder, "data")
    for d in (saves, pictures):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    shutil.rmtree(data_dir, ignore_errors=True)
    views = []
    for p, place in enumerate(places):
        for k in range(args.headings):
            heading = place[3] + 2 * math.pi * k / args.headings
            heading = math.atan2(math.sin(heading), math.cos(heading))
            i = len(views)
            save = os.path.join(saves, "view%02d.kf4quick" % i)
            write_made_up(base, save, number, place[0], place[1], place[2], heading)
            views.append({"place": "p%d" % p, "where": place[4], "asked": [round(v, 1) for v in place[:3]],
                          "heading": heading, "save": save, "load_at": FIRST_LOAD + i * STRIDE if i else 0,
                          "picture_at": FIRST_LOAD + i * STRIDE + SETTLE})
    last = views[-1]["picture_at"] + 2
    loads = ",".join(("%d:%s" % (v["load_at"], v["save"])) if v["load_at"] else v["save"] for v in views)
    pad = os.path.join(folder, "title.pad")
    with open(pad, "w") as f:
        f.write("100 CROSS 4     # the memory card notice; the first view loads on the title screen\n")
        if args.turn:
            f.write("%d RIGHT %d     # (--turn) turning all along\n" % (FIRST_LOAD, last))
    log = os.path.join(folder, "log.txt")
    cmd = [args.exe, "--disc", args.disc, "--no-audio", "--fast", "--headless", "--frames", str(last), "--clock", CLOCK,
           "--memcard", "none", "--input", pad, "--quick-load", loads, "--stats", "--log", log,
           "--dump-frames", ",".join(str(v["picture_at"]) for v in views), "--dump-dir", pictures,
           "--renderer", args.renderer] + (args.extra.split() if args.extra else [])
    if not args.no_capture:
        cmd += ["--gs-capture-at", str(views[0]["picture_at"])]
    env = dict(os.environ, KF4_DATA_DIR=data_dir)
    started = time.time()
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                            env=env)
    wall = time.time() - started
    with open(os.path.join(folder, "stderr.txt"), "w") as f:
        f.write(result.stdout)
    out = {"map": number, "name": name, "exit": result.returncode, "wall": round(wall, 1), "vblanks": last}
    m = re.search(r"\[stats\] VU1 ([\d.]+) s: (\d+) runs, (\d+) instructions", result.stdout)
    if m:
        out["vu1_seconds"], out["vu1_runs"] = float(m.group(1)), int(m.group(2))
    m = re.search(r"\[stats\] VU1 cycles (\d+)", result.stdout)
    if m:
        out["vu1_cycles_run"] = int(m.group(1))
    out["todo"] = sorted(set(re.findall(r"^\[todo\] (.*?) \(.*\)$", result.stdout, re.M)))
    out["fatal"] = re.findall(r"^\[fatal\].*$", result.stdout, re.M)
    out["kernel"] = re.findall(r"^\[kernel\].*$", result.stdout, re.M)[:4]
    out["quick"] = re.findall(r"^\[quick\] (cannot.*|loaded, but.*)$", result.stdout, re.M)

    states, events = parse_log(log) if os.path.exists(log) else ({}, [])
    loaded = [(v, e) for v, e in events if e.startswith("quick loaded: ")]
    floors = [(v, e) for v, e in events if e.startswith("quick load: made up")]
    neighbours = [(v, e) for v, e in events if e.startswith("quick load: neighbouring") or
                  e.startswith("quick load: no neighbouring")]
    for i, view in enumerate(views):
        flags = []
        view["loaded"] = loaded[i][1] if i < len(loaded) else None
        view["loaded_at"] = loaded[i][0] if i < len(loaded) else None
        view["floor"] = floors[i][1] if i < len(floors) else None
        view["neighbour"] = neighbours[i][1][len("quick load: "):] if i < len(neighbours) else None
        if view["loaded"] is None:
            flags.append("not loaded")
        elif view["loaded_at"] > view["picture_at"] - 60:
            flags.append("late")
        if view["floor"] and "no floor" in view["floor"]:
            flags.append("no floor")
        st = state_at(states, view["picture_at"])
        view["state"] = st
        if st:
            if st["empty"] or int(st["map"], 16) != number:
                flags.append("map %s" % st["map"])
            if any(math.isnan(v) or math.isinf(v) for v in st["pos"]):
                flags.append("position NaN")
            if st["death"] or st["hp"] <= 0:
                flags.append("dying")
            if st["status"] != "00000000":
                flags.append("status %s" % st["status"])
        picture = os.path.join(pictures, "frame_%06d.png" % view["picture_at"])
        if os.path.exists(picture):
            view["picture"] = picture
            view["look"] = picture_stats(picture)
            if view["look"]["mean"] < 4:
                flags.append("black")
            elif view["look"]["flat"] > 0.5:
                flags.append("flat %d%%" % (view["look"]["flat"] * 100))
        else:
            flags.append("no picture")
        view["flags"] = flags
    out["views"] = views
    out["loaded_ok"] = sum(1 for v in views if v["loaded"] and int((v["state"] or {"map": "-1"})["map"], 16) == number)
    contact_sheet(views, os.path.join(folder, "sheet.png"), "map %03d %s" % (number, name or ""))

    # the GS frame capture: the census, and VU1 again
    captures = os.path.join(data_dir, "captures")
    found = sorted(os.listdir(captures)) if os.path.isdir(captures) else []
    if found and args.gsreplay:
        capture = os.path.join(folder, "capture.kf4gs")
        shutil.move(os.path.join(captures, found[0]), capture)
        out["capture"] = capture
        listing = subprocess.run([args.gsreplay, capture, "--list"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace")
        with open(os.path.join(folder, "draws.txt"), "w") as f:
            f.write(listing.stdout)
        out["census"] = census(listing.stdout.splitlines())
        vif = subprocess.run([args.gsreplay, capture, "--mode", "vif", "--diff", "0"], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace")
        m = re.search(r"VU1: (\d+) runs, (\d+) instructions, (\d+) cycles", vif.stdout)
        if m:
            out["vu1_frame"] = {"runs": int(m.group(1)), "instructions": int(m.group(2)), "cycles": int(m.group(3))}
        m = re.search(r"VIF1 -> VU1: (\d+) packets as recorded, (\d+) differ, (\d+) recorded but not made, "
                      r"(\d+) made but not recorded", vif.stdout)
        if m:
            out["vif_replay"] = {"same": int(m.group(1)), "differ": int(m.group(2)), "missing": int(m.group(3)),
                                 "extra": int(m.group(4))}
    shutil.rmtree(saves, ignore_errors=True)  # game state; made again each run
    if args.keep_lists:
        # the PC renderer's list of the pipelines it drew with (renderer state only: tools/pipeline_seed.py)
        made = os.path.join(data_dir, "pc_pipeline_list.bin")
        if os.path.isfile(made):
            os.makedirs(args.keep_lists, exist_ok=True)
            shutil.copyfile(made, os.path.join(args.keep_lists, "%03d.bin" % number))
    shutil.rmtree(data_dir, ignore_errors=True)
    return out


# ---- The report ---------------------------------------------------------------------------------
def unusual(m, base_todo):
    notes = []
    if m["exit"] != 0:
        notes.append("exit %d" % m["exit"])
    notes += m["fatal"] + m["kernel"] + m["quick"]
    new_todo = [t for t in m["todo"] if t not in base_todo]
    if new_todo:
        notes.append("new [todo]: " + "; ".join(new_todo))
    for i, v in enumerate(m["views"]):
        if v["flags"]:
            notes.append("view %d (%s %s, h %.2f): %s" % (i, v["place"], v["where"], v["heading"], ", ".join(v["flags"])))
    notes += (m.get("census") or {}).get("odd", [])[:4]
    vf = m.get("vu1_frame")
    if vf and vf["cycles"] > 2 * 4_900_000:  # 4.9M VU1 cycles: a 30 Hz frame at 147 MHz
        notes.append("VU1 %.1fM cycles in the captured frame" % (vf["cycles"] / 1e6))
    vr = m.get("vif_replay")
    if vr and (vr["differ"] or vr["missing"] or vr["extra"]):
        notes.append("VIF replay: %d packets differ, %d missing, %d extra" % (vr["differ"], vr["missing"], vr["extra"]))
    return notes


def write_report(results, out, base_todo):
    total_programs, total_combos = {}, {}
    for m in results:
        c = m.get("census")
        if not c:
            continue
        for p, n in c["programs"].items():
            total_programs.setdefault(p, {})[m["map"]] = n
        for k, n in c["combos"].items():
            total_combos.setdefault(k, {})[m["map"]] = n
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump({"maps": results, "programs": {k: {str(m): n for m, n in v.items()} for k, v in total_programs.items()},
                   "combos": {k: {str(m): n for m, n in v.items()} for k, v in total_combos.items()},
                   "baseline_todo": sorted(base_todo)}, f, indent=1, default=str)

    md = ["# Map tour", "", "%d maps. Per map: views loaded on the map / views, the captured frame's draws, "
          "VU1 runs and cycles (kf4-gsreplay --mode vif), wall seconds for the run, and what was unusual." % len(results),
          "", "| map | name | loaded | draws | VU1 runs | VU1 Mcycles | wall s | unusual |", "|---|---|---|---|---|---|---|---|"]
    for m in results:
        c, vf = m.get("census") or {}, m.get("vu1_frame") or {}
        md.append("| %03d | %s | %d/%d | %s | %s | %s | %s | %s |" % (
            m["map"], m.get("name") or "", m["loaded_ok"], len(m["views"]), c.get("draws", "-"), vf.get("runs", "-"),
            "%.2f" % (vf["cycles"] / 1e6) if vf else "-", m["wall"], "<br>".join(unusual(m, base_todo)) or ""))
    md += ["", "## VU1 programs and paths (draws in the captured frames, by map)", "",
           "| program | maps | draws |", "|---|---|---|"]
    for p in sorted(total_programs, key=lambda p: -len(total_programs[p])):
        maps = total_programs[p]
        md.append("| %s | %d: %s | %d |" % (p, len(maps), " ".join("%03d" % n for n in sorted(maps)), sum(maps.values())))
    md += ["", "## GS state combinations", "",
           "Program, PRIM, TEX0 PSM, ALPHA (ABCD/FIX), TEST (alpha test/method, depth test/method, destination "
           "alpha), ZBUF PSM and mask, FRAME FBP/PSM.", "", "| combination | maps | draws |", "|---|---|---|"]
    for k in sorted(total_combos, key=lambda k: (-len(total_combos[k]), k)):
        maps = total_combos[k]
        shown = " ".join("%03d" % n for n in sorted(maps)) if len(maps) <= 12 else "(%d maps)" % len(maps)
        md.append("| `%s` | %s | %d |" % (k, shown, sum(maps.values())))
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")

    rows = []
    for m in results:
        c, vf = m.get("census") or {}, m.get("vu1_frame") or {}
        notes = unusual(m, base_todo)
        rows.append("<tr><td>%03d</td><td>%s</td><td>%d/%d</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
                    "<td>%s</td></tr><tr><td colspan=8><a href='%03d/sheet.png'><img src='%03d/sheet.png' "
                    "width=640 loading=lazy></a></td></tr>" % (
                        m["map"], html.escape(m.get("name") or ""), m["loaded_ok"], len(m["views"]),
                        c.get("draws", "-"), vf.get("runs", "-"), "%.2f" % (vf["cycles"] / 1e6) if vf else "-",
                        m["wall"], "<br>".join(html.escape(n) for n in notes), m["map"], m["map"]))
    page = ("<!doctype html><meta charset=utf-8><title>Map tour</title><style>body{font:13px sans-serif;"
            "background:#111;color:#ddd}td{vertical-align:top;padding:2px 6px;border-bottom:1px solid #333}"
            "a{color:#9cf}</style><h1>Map tour</h1><p>See report.md for the VU1 program and GS state census.</p>"
            "<table><tr><th>map<th>name<th>loaded<th>draws<th>VU1 runs<th>VU1 Mcycles<th>wall s<th>unusual</tr>%s"
            "</table>" % "".join(rows))
    with open(os.path.join(out, "report.html"), "w", encoding="utf-8") as f:
        f.write(page)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", required=True, help="the output folder (outside the repository)")
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--gsreplay")
    parser.add_argument("--maps", help="map numbers, e.g. 0,99,112 (default: all 64)")
    parser.add_argument("--positions", type=int, default=2, help="places per map (default 2: the start and one more)")
    parser.add_argument("--headings", type=int, default=4, help="headings per place (default 4)")
    parser.add_argument("--jobs", type=int, default=3, help="maps run at once (default 3)")
    parser.add_argument("--base", help="the quick save to start from (default: made from tests/newgame.pad)")
    parser.add_argument("--no-capture", action="store_true", help="no GS capture, so no census")
    parser.add_argument("--renderer", default="pc", choices=("gs", "pc"),
                        help="who draws the pictures (default pc, the PC renderer; gs: paraLLEl-GS)")
    parser.add_argument("--extra", help="more arguments for each run, e.g. \"--native-scale 3 --native-msaa 2\"")
    parser.add_argument("--keep-lists", help="a folder for each map's pc_pipeline_list.bin (NNN.bin), the pipelines "
                        "the PC renderer drew with (tools/pipeline_seed.py)")
    parser.add_argument("--turn", action="store_true", help="turn right all along (the views are then not the same "
                             "each tour, and the pictures between are drawn moving)")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("map_tour: pass --disc or set KF4_DISC")
    args.exe = args.exe or find_exe("kf4")
    args.gsreplay = args.gsreplay or find_exe("kf4-gsreplay")
    if not args.exe:
        sys.exit("map_tour: no built program under out/; build one or pass --exe")
    if args.positions * args.headings > 64:
        sys.exit("map_tour: at most 64 views a map")
    out = os.path.abspath(args.out)
    if out == ROOT or out.startswith(ROOT + os.sep):
        sys.exit("map_tour: --out must be outside the repository (the pictures and captures are the game's)")
    os.makedirs(out, exist_ok=True)

    disc = Disc(args.disc)
    numbers = disc.maps()
    if args.maps:
        wanted = {int(n) for n in args.maps.split(",")}
        numbers = [n for n in numbers if n in wanted]
    names = map_names()
    base_path, base_output = make_base(args.exe, args.disc, out, args.base)
    base = read_base(base_path)
    # the [todo]s a new game prints: only others are news
    base_todo = set(re.findall(r"^\[todo\] (.*?) \(.*\)$", base_output, re.M))

    jobs = []
    for n in numbers:
        places = map_places(disc.read("%03d.map" % n), args.positions)
        jobs.append((n, names.get(n), places))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_map, args, base, n, name, places, os.path.join(out, "%03d" % n)): n
                   for n, name, places in jobs}
        for future in concurrent.futures.as_completed(futures):
            m = future.result()
            results.append(m)
            notes = unusual(m, base_todo)
            print("map %03d: %d/%d views on the map, %s draws, %.0f s%s" % (
                m["map"], m["loaded_ok"], len(m["views"]), (m.get("census") or {}).get("draws", "-"), m["wall"],
                (": " + "; ".join(notes)) if notes else ""), flush=True)
    results.sort(key=lambda m: m["map"])
    write_report(results, out, base_todo)
    print("map_tour: %d maps; %s" % (len(results), os.path.join(out, "report.html")))


if __name__ == "__main__":
    main()
