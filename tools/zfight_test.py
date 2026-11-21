#!/usr/bin/env python3
"""Z-fighting regression test: surfaces that flicker or stitch through each other (decals and overlays on walls and
floors, rock edges, bridge planks, far chests and signs) at spots where they were seen (docs/port/ZFIGHT_TEST.md).

    tools/zfight_test.py [--spots DIR] [--spot NAME,...] [--exe KF4] [--disc ISO] [--out DIR] [--jobs 1|2]
                         [--keep-frames] [--approve] [-- extra kf4 options]

A spot is a small JSON file, tests/zspots/NAME.json by default (--spots, or KF4_ZSPOTS): a map, the player's
position, heading and pitch, and how to look (see ZFIGHT_TEST.md for every field). It holds no game data. At run
time the tool makes a real quick save of a new game with the program under test (tests/newgame.pad,
--quick-save-at; cached in OUT/base), then writes from it one "made-up" quick save per view (map_tour.py's: the
map tag, position and rotation changed; the game puts the player on the floor and loads the neighbouring map), and
plays them all in one headless, silent run (--quick-load A,V:B,...), a picture of each once it has settled.

The motions (the spot's "motion", all by default): turn (the heading turned 0.02 degrees a view: 0.4 pixels at 2x),
step and side (moved 0.5 units a view forward, sideways), each a sweep of 9 views with the middle view before and
after it; and walk (the game's own walk forward and back by pad presses, from one load, every picture at 240 with
the pictures between, played twice). What it measures:
- in pictures that only move smoothly, a pixel's colour lies between (or near) its colours in the picture before
  and the picture after;
- where two surfaces fight over depth, the winner changes with the tiniest move, so the pixel jumps away from both
  its neighbours and back: a flip (above both, or below both, by more than a threshold, in some channel);
- robust measure: the neighbours' colours taken within `radius` (1) pixels and 12 levels, so that an aliased edge
  crossing a pixel does not count; fine measure: the neighbours' own pixel and 24 levels (1-pixel stripes count);
  walk: within 2 pixels, 24 levels;
- what changes with time and not with the view (the HUD's orb, fire, water, creatures) differs between the middle
  view and its repeats (or between the walk's two plays) and is left out, as is the HUD's corner.
Each measure counts flip pixels per 100,000 pixels of the picture against its limit (max_score 25, fine_max 40,
walk_max 25); a spot's score is the worst of them in percent of its limit, and it passes at 100% or less. No
approved pictures: lighting, fog and filtering changes move colours smoothly and do not flip.

Into OUT/NAME/: evidence.png (the worst part: the middle picture, its flip map, and the whole picture with the
flips marked), score.json, run.log. The pictures are deleted after scoring unless --keep-frames. OUT/report.md
and report.json: a line a spot. Exit code 1 when a spot fails or cannot be measured. Needs numpy and Pillow.

--approve writes each spot's score into its JSON as "approved" (with the build), as a record of what a good build
scored there; it changes no limit.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NEWGAME_PAD = os.path.join(ROOT, "tests", "newgame.pad")
DEFAULT_SPOTS = os.path.join(ROOT, "tests", "zspots")
CLOCK = "2026-01-01T12:00:00"


# the defaults of a spot's fields (ZFIGHT_TEST.md)
SPOT_DEFAULTS = {
    "pitch": 0.0,
    "views": 9,            # views in the sweep (and the middle one again)
    "motion": "all",       # turn, step, side, walk, a list of them, or all
    "step": 0.02,          # turn: degrees between views
    "step_units": 0.5,     # step and side: the game's units between views
    "walk": [["UP", 4, 4], ["DOWN", 4, 4], ["UP", 4, 4], ["DOWN", 4, 6]],  # walk: [button, held, rest] vblanks
    "walk_radius": 2,      # walk: pixels a picture's content may move from its neighbour's
    "walk_threshold": 24,  # walk: levels a pixel must stand out by
    "walk_max": 25,        # walk: flip pixels per 100,000 allowed
    "axis": "heading",     # heading or pitch
    "scale": 2,            # --native-scale
    "frame_rate": "240",
    "threshold": 12,       # robust measure: levels (of 255) a pixel must stand out from both neighbours by,
    "radius": 1,           #   the neighbours' colours taken within this many pixels (aliased edges do not count)
    "max_score": 25,       #   flip pixels per 100,000 allowed
    "fine_threshold": 24,  # fine measure: levels a pixel must stand out by, the neighbours' own pixel (1-pixel
    "fine_max": 40,        #   stripes count; aliased edges too, so a higher limit)
    "region": None,        # [x0, y0, x1, y1] in fractions of the picture; default all of it
    "ignore": [],          # more [x0, y0, x1, y1] boxes left out
    "args": [],            # more kf4 options
    "env": {},             # more environment
    "teleport_first": None,  # a map to visit before the spot (state that lasts across maps)
    "settle": 90,          # vblanks from a view's load to its picture (the fade in is about 70)
}
# the owner's picture settings (settings.json); an option the program does not know (older builds) is left out
OWNER = ["--native-draw", "on", "--widescreen", "--native-widescreen", "--fov", "64", "--antialias", "fxaa",
         "--native-aniso", "16", "--native-lod", "full", "--native-layer-z", "tolerant",
         "--native-msaa", "4", "--native-cutout-aa", "ssaa"]  # (builds before FXAA, 2026-11-16: MSAA, as played then)
HUD = [(0.0, 0.0, 0.29, 0.135), (0.0, 0.0, 0.125, 0.235)]  # the gauges' corner (widescreen HUD in the corner)
MOTIONS = ("turn", "step", "side", "walk")

FIRST_LOAD = 130  # the first view is loaded on the title screen (map_tour.py's)
STRIDE = 100      # vblanks a view

# ---- quick saves (port/platform/quicksave.c, game/quicksave.c; map_tour.py's made-up files) ----------------------
QUICK_HEADER_SIZE = 40
QUICK_INFO_SIZE = 68
IMAGE_MAGIC = 0x15400
IMAGE_PLAYER = 0x13FB0   # PlayerState: the position (x, y, z, w), then at 0x10 the rotation (pitch, heading, roll)
IMAGE_MAP_TAG = 0x14018
FLAG_MADE_UP = 2


def fnv1a(data, h=0xCBF29CE484222325):
    for byte in data:
        h = ((h ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def read_quick(path):
    """A quick save's parts: (header, info, image)"""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"KF4QUICK":
        raise ValueError("%s is not a quick save" % path)
    (info_size,) = struct.unpack_from("<I", data, 24)
    image = bytearray(data[QUICK_HEADER_SIZE + info_size:])
    if struct.unpack_from("<I", image, 0)[0] != IMAGE_MAGIC:
        raise ValueError("%s holds no save image" % path)
    return data[:QUICK_HEADER_SIZE], data[QUICK_HEADER_SIZE:QUICK_HEADER_SIZE + info_size], image


def quick_place(path):
    """Where a quick save stands: map, position, heading, pitch (from the game's save image)"""
    _, _, image = read_quick(path)
    x, y, z = struct.unpack_from("<3f", image, IMAGE_PLAYER)
    pitch, heading, _ = struct.unpack_from("<3f", image, IMAGE_PLAYER + 0x10)
    (tag,) = struct.unpack_from("<i", image, IMAGE_MAP_TAG)
    return {"map": tag, "position": [x, y, z], "heading": heading, "pitch": pitch}


def write_made_up(base, path, tag, position, heading, pitch):
    """base's save image with the map, position and rotation changed, marked made up (map_tour.py's)"""
    header, info, image = base
    image = bytearray(image)
    (size,) = struct.unpack_from("<I", image, 4)
    struct.pack_into("<3f", image, IMAGE_PLAYER, *position)
    struct.pack_into("<3f", image, IMAGE_PLAYER + 0x10, pitch, heading, 0.0)
    struct.pack_into("<i", image, IMAGE_MAP_TAG, tag)
    for block in range(size // 128):  # Game_CheckSaveImage: a byte sum per 128 bytes
        image[size + block] = sum(image[block * 128:(block + 1) * 128]) & 0xFF
    info = bytearray(info[:64]) + bytes([0xFF, 0xFF, 0xFF, FLAG_MADE_UP])
    struct.pack_into("<i3f3f", info, 8, tag, *position, pitch, heading, 0.0)
    header = bytearray(header)
    struct.pack_into("<I", header, 24, QUICK_INFO_SIZE)
    struct.pack_into("<Q", header, 32, fnv1a(bytes(image), fnv1a(bytes(info))))
    with open(path, "wb") as f:
        f.write(bytes(header) + bytes(info) + bytes(image))


def write_moved(quick, path, d_position, d_heading, d_pitch):
    """(spots with "quick") a real quick save with the player moved and turned a little: all else as he had it"""
    header, info, image = read_quick(quick)
    image = bytearray(image)
    info = bytearray(info)
    (size,) = struct.unpack_from("<I", image, 4)
    position = [p + d for p, d in zip(struct.unpack_from("<3f", image, IMAGE_PLAYER), d_position)]
    pitch, heading, roll = struct.unpack_from("<3f", image, IMAGE_PLAYER + 0x10)
    pitch, heading = pitch + d_pitch, heading + d_heading
    struct.pack_into("<3f", image, IMAGE_PLAYER, *position)
    struct.pack_into("<3f", image, IMAGE_PLAYER + 0x10, pitch, heading, roll)
    for block in range(size // 128):
        image[size + block] = sum(image[block * 128:(block + 1) * 128]) & 0xFF
    struct.pack_into("<3f3f", info, 8 + 4, *position, pitch, heading, roll)
    header = bytearray(header)
    struct.pack_into("<Q", header, 32, fnv1a(bytes(image), fnv1a(bytes(info))))
    with open(path, "wb") as f:
        f.write(bytes(header) + bytes(info) + bytes(image))


# ---- the program ------------------------------------------------------------------------------------------------
def find_exe():
    for preset in ("linux-clang", "windows-clang"):
        for name in ("kf4", "kf4.exe"):
            path = os.path.join(ROOT, "out", preset, name)
            if os.path.exists(path):
                return path
    sys.exit("zfight_test: no program in out/linux-clang or out/windows-clang; build it or pass --exe")


_help_cache = {}


def known_options(exe):
    """The options the program knows (older builds lack some of today's): each option's name is a string in the
    program (main_host.c's strcmp), as --help may not name them all"""
    if exe not in _help_cache:
        with open(exe, "rb") as f:
            data = f.read()
        _help_cache[exe] = set(c.decode() for c in data.split(bytes(1))
                               if re.fullmatch(rb"--[a-z0-9][a-z0-9-]*", c))
    return _help_cache[exe]


def compatible(exe, args):
    """args without the options (and their values) the program does not know; --saves none as --memcard none for
    builds from before the saves were files"""
    known = known_options(exe)
    out, i = [], 0
    while i < len(args):
        a = args[i]
        has_value = i + 1 < len(args) and not args[i + 1].startswith("--")
        if a.startswith("--") and a not in known:
            if a == "--saves" and "--memcard" in known:
                out += ["--memcard"] + ([args[i + 1]] if has_value else [])
            i += 2 if has_value else 1
            continue
        out.append(a)
        i += 1
    return out


def make_base(exe, disc, folder):
    """A real quick save of a new game, made by this program (cached by the program's size and time)"""
    st = os.stat(exe)
    key = hashlib.sha1(("%s|%d|%d" % (os.path.abspath(exe), st.st_size, int(st.st_mtime))).encode()).hexdigest()[:12]
    path = os.path.join(folder, "base_%s.kf4quick" % key)
    if os.path.exists(path):
        return path
    os.makedirs(folder, exist_ok=True)
    data_dir = tempfile.mkdtemp(prefix="base_data_", dir=folder)
    cmd = compatible(exe, [exe, "--disc", disc, "--no-audio", "--fast", "--headless", "--frames", "1950", "--clock",
                           CLOCK, "--saves", "none", "--input", NEWGAME_PAD, "--quick-save-at", "1900:" + path])
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                       env=dict(os.environ, KF4_DATA_DIR=data_dir))
    shutil.rmtree(data_dir, ignore_errors=True)
    if r.returncode != 0 or not os.path.exists(path):
        sys.stdout.write(r.stdout[-3000:])
        sys.exit("zfight_test: could not take the base quick save")
    return path


# ---- spots ----------------------------------------------------------------------------------------------------
def load_spot(path):
    with open(path) as f:
        spot = json.load(f)
    for k, v in SPOT_DEFAULTS.items():
        spot.setdefault(k, v)
    spot["name"] = os.path.splitext(os.path.basename(path))[0]
    spot["path"] = path
    if "quick" in spot and not os.path.isabs(spot["quick"]):
        spot["quick"] = os.path.join(os.path.dirname(path), spot["quick"])
    if "quick" not in spot and not all(k in spot for k in ("map", "position", "heading")):
        raise ValueError("%s: a spot needs map, position and heading (or quick)" % path)
    return spot


def list_spots(folder, names):
    files = sorted(f for f in os.listdir(folder) if f.endswith(".json"))
    spots = [load_spot(os.path.join(folder, f)) for f in files]
    if names:
        want = names.split(",")
        missing = [n for n in want if n not in {s["name"] for s in spots}]
        if missing:
            sys.exit("zfight_test: no spot %s in %s" % (", ".join(missing), folder))
        spots = [s for s in spots if s["name"] in want]
    return spots


def motions(spot):
    m = spot["motion"]
    kinds = MOTIONS if m == "all" else ([m] if isinstance(m, str) else list(m))
    bad = [k for k in kinds if k not in MOTIONS]
    if bad:
        raise ValueError("%s: motion %s is none of %s or all" % (spot["path"], ", ".join(bad), ", ".join(MOTIONS)))
    return kinds


def sweep(n, step):
    """n offsets of step around 0, with the middle one before and after them as well (for the animated mask)"""
    s = [step * (i - (n - 1) / 2.0) for i in range(n)]
    return [s[n // 2]] + s + [s[n // 2]]


def plan_segments(spot):
    """The parts of the run: each a list of views (a move and turn from the spot) and how its pictures are taken.
    turn: the heading (or pitch, axis) turned by `step` degrees a view; step: moved forward and back by `step_units`
    a view; side: moved sideways so; walk: the game's own walk forward and back (pad presses, the walk's speeding up
    and the view's bob), a picture every vblank, played twice from the same load (what differs between the two
    plays is animated)."""
    h = float(spot.get("heading", 0.0))
    forward = (math.sin(h), 0.0, math.cos(h))  # (the game's: x grows with sin(heading), z with cos)
    side = (math.cos(h), 0.0, -math.sin(h))
    n = int(spot["views"])
    segments = []
    for kind in motions(spot):
        if kind == "turn":
            views = [((0, 0, 0), d, 0.0) if spot["axis"] == "heading" else ((0, 0, 0), 0.0, d)
                     for d in sweep(n, math.radians(float(spot["step"])))]
            segments.append({"kind": kind, "views": views})
        elif kind in ("step", "side"):
            v = forward if kind == "step" else side
            views = [((v[0] * d, 0.0, v[2] * d), 0.0, 0.0) for d in sweep(n, float(spot["step_units"]))]
            segments.append({"kind": kind, "views": views})
        elif kind == "walk":
            segments.append({"kind": kind, "views": [((0, 0, 0), 0.0, 0.0)] * 2})
    return segments


def walk_presses(start, walk):
    """The pad lines of one play of the walk from vblank start, and its length: each [button, vblanks held,
    vblanks of rest after]"""
    lines, t = [], start
    for button, held, rest in walk:
        lines.append("%d %s %d" % (t, button, held))
        t += held + rest
    return lines, t - start


def run_spot(args, spot, base_path):
    folder = os.path.join(args.out, spot["name"])
    shutil.rmtree(folder, ignore_errors=True)
    saves, pictures, data_dir = (os.path.join(folder, d) for d in ("saves", "pictures", "data"))
    for d in (saves, pictures, data_dir):
        os.makedirs(d)
    base = read_quick(base_path) if "quick" not in spot else None
    settle = int(spot["settle"])
    t = FIRST_LOAD
    if spot["teleport_first"] is not None:
        t += 700  # into play with the first view, then the other map (state that lasts across maps), then the views
    pad_lines = ["100 CROSS 4     # the notice that there is nowhere to save; the first load is on the title screen"]
    loads, dumps = [], []
    segments = plan_segments(spot)
    for seg in segments:
        seg["pictures"] = []
        for i, (move, dh, dp) in enumerate(seg["views"]):
            path = os.path.join(saves, "%s%02d.kf4quick" % (seg["kind"], i))
            if seg["kind"] == "walk" and i == 1:
                path = os.path.join(saves, "walk00.kf4quick")
            elif base is not None:
                position = [float(p) + d for p, d in zip(spot["position"], move)]
                write_made_up(base, path, int(spot["map"]), position, float(spot["heading"]) + dh,
                              float(spot["pitch"]) + dp)
            else:
                write_moved(spot["quick"], path, move, dh, dp)
            loads.append((t, path))
            if seg["kind"] == "walk":
                lines, length = walk_presses(t + settle, spot["walk"])
                pad_lines += lines
                shots = list(range(t + settle - 2, t + settle + length + 2))
                seg["pictures"].append(shots)
                dumps += shots
                t += settle + length + 20
            else:
                seg["pictures"].append(t + settle)
                dumps.append(t + settle)
                t += STRIDE
    if spot["teleport_first"] is not None:
        loads.insert(0, (0, loads[0][1]))
    # (the first load is taken on the title screen, with no vblank)
    quick = ",".join(p if k == 0 else "%d:%s" % (at, p) for k, (at, p) in enumerate(loads))
    pad = os.path.join(folder, "spot.pad")
    with open(pad, "w") as f:
        f.write("\n".join(pad_lines) + "\n")
    frames = max(dumps) + 2
    cmd = [args.exe, "--disc", args.disc, "--headless", "--no-audio", "--fast", "--frames", str(frames),
           "--clock", CLOCK, "--saves", "none", "--input", pad, "--quick-load", quick,
           "--dump-frames", ",".join(map(str, dumps)), "--dump-dir", pictures,
           "--native-scale", str(spot["scale"]), "--frame-rate", str(spot["frame_rate"])] + OWNER
    if spot["teleport_first"] is not None:
        cmd += ["--teleport", "%d:%s" % (FIRST_LOAD + 300, spot["teleport_first"] if ":" in str(spot["teleport_first"])
                                      else "%s:0" % spot["teleport_first"]), "--invincible"]
    cmd = compatible(args.exe, cmd + list(spot["args"]) + list(args.extra))
    env = dict(os.environ, KF4_DATA_DIR=data_dir)
    env.update({k: str(v) for k, v in spot["env"].items()})
    started = time.time()
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", env=env)
    wall = time.time() - started
    with open(os.path.join(folder, "run.log"), "w") as f:
        f.write(" ".join(cmd) + "\n\n" + r.stdout)
    shutil.rmtree(data_dir, ignore_errors=True)
    shutil.rmtree(saves, ignore_errors=True)
    result = {"spot": spot["name"], "exit": r.returncode, "seconds": round(wall, 1)}
    floors = re.findall(r"quick load: made up: (.*)", r.stdout)
    if floors:
        result["floor"] = floors[0]
    picture = lambda v: os.path.join(pictures, "frame_%06d.png" % v)  # noqa: E731

    def all_pictures(v):
        """the vblank's picture and, at higher frame rates, its pictures between (frame_V_250.png ...)"""
        between = [f for f in os.listdir(pictures) if f.startswith("frame_%06d_" % v)]
        return [picture(v)] + [os.path.join(pictures, f) for f in sorted(between, key=lambda f: int(f[13:-4]))]
    missing = [v for v in dumps if not os.path.exists(picture(v))]
    if r.returncode != 0 or missing:
        result.update(status="ERROR", error="exit %d, %d of %d pictures" % (r.returncode, len(dumps) - len(missing),
                                                                          len(dumps)))
        return result
    result.update(score=0.0, motions={})
    worst = None
    # what changes with time and not with the view: every sweep's middle view against its repeats, all sweeps
    # together (they are all within a few pixels of each other)
    sweeps = [[picture(v) for v in seg["pictures"]] for seg in segments if seg["kind"] != "walk"]
    animated = animated_mask(sweeps) if sweeps else None
    for seg in segments:
        if seg["kind"] == "walk":
            m = score_walk([p for v in seg["pictures"][0] for p in all_pictures(v)],
                           [p for v in seg["pictures"][1] for p in all_pictures(v)], spot)
        else:
            m = score_sweep([picture(v) for v in seg["pictures"]], animated, spot)
        evidence = m.pop("evidence")
        result["motions"][seg["kind"]] = m
        if worst is None or m["score"] > result["score"]:
            worst, result["score"], result["worst_motion"] = evidence, m["score"], seg["kind"]
    write_evidence(os.path.join(folder, "evidence.png"), spot["name"], result, *worst)
    unsteady = [k for k, m in result["motions"].items() if m["unsteady"]]
    if unsteady:
        result.update(status="ERROR", error="the view does not hold still under %s (a flash, a creature?): "
                      "%s" % (", ".join(unsteady), "; ".join(result["motions"][k]["unsteady"] for k in unsteady)))
    else:
        result["status"] = "PASS" if result["score"] <= 100 else "FAIL"
    if not args.keep_frames:
        shutil.rmtree(pictures, ignore_errors=True)
    with open(os.path.join(folder, "score.json"), "w") as f:
        json.dump(result, f, indent=1)
    return result


# ---- the measure ------------------------------------------------------------------------------------------------
def box_mask(shape, box, value):
    h, w = shape
    x0, y0, x1, y1 = box
    m = np.zeros(shape, bool)
    m[int(round(y0 * h)):int(round(y1 * h)), int(round(x0 * w)):int(round(x1 * w))] = True
    return m if value else ~m


def dilate(mask, r):
    """mask grown by r pixels (a square)"""
    out = mask.copy()
    for axis in (0, 1):
        grown = out.copy()
        for d in range(1, r + 1):
            grown |= np.roll(out, d, axis=axis) | np.roll(out, -d, axis=axis)
        out = grown
    return out


def window(img, r, op):
    """img's max (op np.maximum) or min (np.minimum) over a (2r+1) square, per channel"""
    out = img
    for axis in (0, 1):
        grown = out.copy()
        for d in range(1, r + 1):
            grown = op(grown, op(np.roll(out, d, axis=axis), np.roll(out, -d, axis=axis)))
        out = grown
    return out


def read(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def flip_map(paths, threshold, radius):
    """Per pixel: how many pictures stand out from both their neighbours (above both, or below both, in a channel)
    by more than threshold, the neighbours' colours taken from a (2*radius+1) square around the pixel (the most a
    pixel's content moves between pictures); and the largest such jump"""
    count = largest = None
    win = []
    for p in paths:
        img = read(p)
        win.append((img, window(img, radius, np.maximum), window(img, radius, np.minimum)) if radius else
                   (img, img, img))
        if len(win) < 3:
            continue
        (_, amax, amin), (b, _, _), (_, cmax, cmin) = win
        win.pop(0)
        above = b - np.maximum(amax, cmax)
        below = np.minimum(amin, cmin) - b
        o = np.maximum(above, below).max(axis=2)
        if count is None:
            count = np.zeros(o.shape, np.int16)
            largest = np.zeros(o.shape, np.int16)
        count += o > threshold
        np.maximum(largest, o, out=largest)
    return count, largest


def counted_region(shape, spot):
    region = box_mask(shape, spot["region"], True) if spot["region"] else np.ones(shape, bool)
    for box in HUD + [tuple(b) for b in spot["ignore"]]:
        region &= box_mask(shape, box, False)
    return region


def measure(count, largest, animated, middle, spot):
    shape = middle.shape[:2]
    counted = counted_region(shape, spot) & ~animated
    flips = (count > 0) & counted
    per = 100000.0 / (shape[0] * shape[1])
    b = max(16, 32 * shape[1] // 1920)
    hb, wb = shape[0] // b, shape[1] // b
    blocks = flips[:hb * b, :wb * b].reshape(hb, b, wb, b).sum(axis=(1, 3))
    by, bx = np.unravel_index(int(np.argmax(blocks)), blocks.shape)
    m = {
        "flips": round(float(flips.sum()) * per, 2),
        "flip_pixels": int(flips.sum()),
        "flips_twice": int(((count > 1) & counted).sum()),
        "animated_pct": round(100.0 * float(animated.sum()) / animated.size, 2),
        "worst_block": [int(bx * b), int(by * b), int(blocks[by, bx])],
        "picture": [shape[1], shape[0]],
        "unsteady": "",
    }
    m["evidence"] = (middle, flips, counted, largest, (bx * b + b // 2, by * b + b // 2))
    return m


def animated_mask(sweeps):
    """Pixels that differ (by more than 12 levels, grown by 2 pixels) between a sweep's middle view and its repeats
    (its first and last pictures), in any sweep"""
    mask = None
    for paths in sweeps:
        middle = read(paths[1 + (len(paths) - 2) // 2])
        for p in (paths[0], paths[-1]):
            d = np.abs(middle - read(p)).max(axis=2) > 12
            mask = d if mask is None else mask | d
    return dilate(mask, 2)


def score_sweep(paths, animated, spot):
    """A turn, step or side sweep: the middle view, the sweep, the middle view again. Two measures: robust (the
    neighbours' colours within `radius` pixels, `threshold` levels) and fine (the neighbours' own pixel,
    `fine_threshold` levels); the motion's score is the larger of each against its limit, in percent"""
    sweep_paths, before, after = paths[1:-1], read(paths[0]), read(paths[-1])
    middle = read(sweep_paths[len(sweep_paths) // 2])
    robust = measure(*flip_map(sweep_paths, float(spot["threshold"]), int(spot["radius"])), animated, middle, spot)
    fine = measure(*flip_map(sweep_paths, float(spot["fine_threshold"]), 0), animated, middle, spot)
    r_score = 100.0 * robust["flips"] / float(spot["max_score"])
    f_score = 100.0 * fine["flips"] / float(spot["fine_max"])
    m = dict(robust if r_score >= f_score else fine)
    m.update(score=round(max(r_score, f_score), 1), robust=robust["flips"], fine=fine["flips"],
             by="robust" if r_score >= f_score else "fine")
    del m["flips"]
    # a view that is not a small move from its neighbour (a flash, a creature in the face)
    region = counted_region(middle.shape[:2], spot)
    grey = lambda img: img.astype(np.float32).mean(axis=2)  # noqa: E731
    flash = max(float(np.abs(grey(middle) - grey(before))[region].mean()),
                float(np.abs(grey(middle) - grey(after))[region].mean()))
    if flash > 4 or m["animated_pct"] > 25:
        m["unsteady"] = "the middle view and its repeats differ by %.1f levels (%.0f%% animated)" % (
            flash, m["animated_pct"])
    return m


def score_walk(play1, play2, spot):
    """The walk: the first play's pictures in turn (with the pictures between at higher frame rates), against both
    neighbours within walk_radius pixels; what differs between the two plays at the same moment is animated"""
    animated = None
    for a, b in zip(play1, play2):
        d = np.abs(read(a) - read(b)).max(axis=2) > 12
        animated = d if animated is None else animated | d
    animated = dilate(animated, 2)
    count, largest = flip_map(play1, float(spot["walk_threshold"]), int(spot["walk_radius"]))
    middle = read(play1[len(play1) // 2])
    m = measure(count, largest, animated, middle, spot)
    m.update(score=round(100.0 * m["flips"] / float(spot["walk_max"]), 1), walk=m.pop("flips"), by="walk")
    if m["animated_pct"] > 25:
        m["unsteady"] = "the two plays of the walk differ at %.0f%% of the picture" % m["animated_pct"]
    return m


def write_evidence(path, name, result, middle, flips, counted, largest, centre):
    """A small picture: the worst part of the worst motion's middle picture, its flips, and the whole picture with
    the flips in red"""
    h, w = flips.shape
    size = min(256, h, w)
    cx = min(max(centre[0] - size // 2, 0), w - size)
    cy = min(max(centre[1] - size // 2, 0), h - size)
    crop = np.clip(middle[cy:cy + size, cx:cx + size], 0, 255).astype(np.uint8)
    heat = np.zeros((size, size, 3), np.uint8)
    heat[..., 0] = np.clip(largest[cy:cy + size, cx:cx + size].astype(np.int32) * 3, 0, 255)
    off = ~counted[cy:cy + size, cx:cx + size]
    heat[off] //= 3
    heat[off, 2] = 60
    zoom = 512 // size
    left = Image.fromarray(crop).resize((size * zoom, size * zoom), Image.NEAREST)
    mid = Image.fromarray(heat).resize((size * zoom, size * zoom), Image.NEAREST)
    whole = np.clip(middle, 0, 255).astype(np.uint8).copy()
    whole[dilate(flips, max(1, w // 960))] = [255, 0, 0]
    tw = 512 * w // h
    thumb = Image.fromarray(whole).resize((tw, 512), Image.BILINEAR)
    s = tw / w
    ImageDraw.Draw(thumb).rectangle([cx * s, cy * s, (cx + size) * s, (cy + size) * s], outline=(255, 255, 0))
    sheet = Image.new("RGB", (1024 + tw, 512 + 20))
    sheet.paste(left, (0, 20))
    sheet.paste(mid, (512, 20))
    sheet.paste(thumb, (1024, 20))
    scores = motion_scores(result)
    ImageDraw.Draw(sheet).text((4, 4), "%s  score %.0f%% (%s)  [%s]  crop at %d,%d" % (
        name, result["score"], result["worst_motion"], scores, cx, cy), fill=(255, 255, 0))
    sheet.save(path)


def motion_scores(r):
    """each motion's score (percent of its limit) and its flips a 100,000 pixels: r robust, f fine, w walk"""
    out = []
    for k, m in r.get("motions", {}).items():
        raw = "w%.1f" % m["walk"] if "walk" in m else "r%.1f f%.1f" % (m["robust"], m["fine"])
        out.append("%s %.0f%% (%s)" % (k, m["score"], raw))
    return ", ".join(out)


# ---- the run ----------------------------------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--spots", default=os.environ.get("KF4_ZSPOTS", DEFAULT_SPOTS),
                        help="the spots' folder (default KF4_ZSPOTS, else tests/zspots)")
    parser.add_argument("--spot", help="only these spots (names, comma-separated)")
    parser.add_argument("--exe")
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--out", help="the output folder (outside the repository; default a temporary one)")
    parser.add_argument("--jobs", type=int, default=1, choices=(1, 2), help="spots run at once (1 or 2)")
    parser.add_argument("--keep-frames", action="store_true", help="keep the pictures (big at high scales)")
    parser.add_argument("--approve", action="store_true",
                        help="write each spot's score into its JSON as \"approved\" (a good build's record)")
    parser.add_argument("extra", nargs="*", help="more kf4 options (after --)")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("zfight_test: pass --disc or set KF4_DISC")
    args.exe = os.path.abspath(args.exe or find_exe())
    args.out = os.path.abspath(args.out or tempfile.mkdtemp(prefix="kf4_zfight_"))
    os.makedirs(args.out, exist_ok=True)
    spots = list_spots(args.spots, args.spot)
    if not spots:
        sys.exit("zfight_test: no spots in %s" % args.spots)
    base = None
    if any("quick" not in s for s in spots):
        base = make_base(args.exe, args.disc, os.path.join(args.out, "base"))
    started = time.time()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [(s, pool.submit(run_spot, args, s, base)) for s in spots]
        for s, fut in futures:
            r = fut.result()
            results.append(r)
            print("%-22s %-5s %6s%%  %s  %s" % (
                r["spot"], r["status"], r.get("score", "-"), motion_scores(r),
                r.get("error") or os.path.join(args.out, r["spot"], "evidence.png")), flush=True)
            if args.approve and r["status"] != "ERROR":
                with open(s["path"]) as f:
                    raw = json.load(f)
                raw["approved"] = {"score": r["score"], "exe": args.exe, "date": datetime.date.today().isoformat()}
                with open(s["path"], "w") as f:
                    json.dump(raw, f, indent=2)
                    f.write("\n")
    total = time.time() - started
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump({"exe": args.exe, "seconds": round(total, 1), "spots": results}, f, indent=1)
    with open(os.path.join(args.out, "report.md"), "w") as f:
        f.write("# Z-fighting test\n\n%s, %.0f s\n\n| spot | status | score (%% of limit) | by motion | evidence |\n"
                "|---|---|---|---|---|\n" % (args.exe, total))
        for r in results:
            f.write("| %s | %s | %s | %s | %s |\n" % (r["spot"], r["status"], r.get("score", "-"), motion_scores(r),
                                                    r.get("error") or "%s/evidence.png" % r["spot"]))
    bad = [r for r in results if r["status"] != "PASS"]
    print("zfight_test: %d of %d spots pass, %.0f s; report in %s" % (len(results) - len(bad), len(results), total,
                                                                      os.path.join(args.out, "report.md")))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
