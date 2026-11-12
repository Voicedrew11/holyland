#!/usr/bin/env python3
"""The PC renderer against paraLLEl-GS in motion: menus, messages, combat, spells, creatures' effects, fades and
under water, each played the same way by both renderers and compared picture by picture, and the PC renderer's
pictures between (240) judged against their game frames.

    tools/motion_compare.py --out DIR [--exe EXE] [--disc ISO] [--cases a,b|list] [--profiles 1x,4xw]
                            [--no-240] [--keep] [--top N] [--level L]

Each case is a pad script played after tests/newgame.pad (tests/motion/*.pad, vblanks counted from the start of
the run), with options (--cheats, --teleport, --invincible) and KF4_FX (game/screen_fx.c: spell=N casts that
spell, creature=ROW puts the player before a creature of that chr record, water forces the under-water screen).
Every run: --headless --no-audio --fast --memcard none, a fresh KF4_DATA_DIR in DIR, one game at a time.

For each case and profile:
1. gs: --renderer gs at the PS2's rate (30), --dump-frames at the case's vblanks, --log;
2. pc: the same with the profile's options;
3. each vblank's two pictures paired (brought to one size) and measured as tools/tour_compare.py does (mean,
   share over --level, the same after a 3 x 3 blur), the game state at that vblank checked the same in both
   runs (the --log line: frame, menu, map, position, heading, HP);
4. (unless --no-240) pc again at --frame-rate 240: its pictures between checked with tools/flash_find.py --frames
   (pictures between not between their game frames), and its game frames (phase 0 of a vblank where the log's
   frame counter moves on) against step 2's pictures.
Pictures are deleted after each step; the worst --top pairs of each case and profile are kept small (640 wide,
PC | GS | difference) in DIR/evidence/, with flash_find's sheet.

Profiles: 1x (--native-scale 1 --no-native-widescreen), 4xw (--native-scale 4 --widescreen --native-widescreen
--native-msaa 2; gs gets --widescreen; the 4x pictures are brought down to gs's 640 x 448). MSAA is passed but
the PC renderer does not apply it yet (docs/port/PC_RENDERER.md).

Into DIR: report.md (a table a case and profile, then each case's worst pictures), report.json (every picture).
The pictures hold the game's data: keep DIR outside the repository. Needs Pillow and numpy.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from tour_compare import measure, pair_picture, pictures_at_one_size  # noqa: E402

EXE = os.path.join(ROOT, "out", "windows-clang" if os.name == "nt" else "linux-clang",
                   "kf4.exe" if os.name == "nt" else "kf4")

PROFILES = {
    "1x": {"pc": ["--native-scale", "1", "--no-native-widescreen"], "gs": []},
    "4xw": {"pc": ["--native-scale", "4", "--widescreen", "--native-widescreen", "--native-msaa", "2"],
            "gs": ["--widescreen"]},
}


def r(a, b, step=1):
    return list(range(a, b + 1, step))


# name -> pad (tests/motion/NAME.pad, or None: newgame.pad alone), args, fx (KF4_FX), vblanks to compare.
# Runs of vblanks one after another are what flash_find needs at 240 (8 or more).
EQUIP = "equip"  # tests/motion/equip.pad: a club put on, then play
CASES = {
    # the logos, the title, Press Start, New Game, the opening movie's end and its messages, the fade into play
    "title": dict(pad=None, args=[], fx="", frames=r(300, 311) + r(400, 411) + r(500, 520, 2) + r(745, 756) +
                  r(800, 811) + r(1500, 1520, 2) + r(1700, 1711)),
    # every page of the menu (tests/motion/menus.pad)
    "menus": dict(pad="menus", args=["--cheats", "all"], fx="",
                  frames=r(1800, 1823) + [1910, 1970, 2030, 2210, 2270, 2450, 2510, 2570, 2630, 2840, 3020, 3080,
                                          3320, 3500, 3590] + r(2400, 2423)),
    # walking to the herb by the start and picking it up: the item drawn large, turning, flying to the player
    # (tests/motion/herb_pickup.pad)
    "pickup": dict(pad="herb_pickup", args=[], fx="", frames=r(1820, 1843) + r(1880, 1903)),
    # the club put on (tests/motion/equip.pad), map 002 (the teleport's fade), a Maneater before the player,
    # swings (the weapon's trail), its attacks, hits, its death or the player's
    "combat": dict(pad="combat", args=["--cheats", "all", "--teleport", "2200:2:0"], fx="creature=0:400",
                   frames=r(2200, 2211) + r(2290, 2301) + r(2500, 2523) + r(2640, 2663) + r(2800, 2823)),
    # spells cast (KF4_FX=spell=N, TRIANGLE every 120 vblanks from 1900; tests/motion/cast.pad). 36, 61 and 76
    # showed nothing in these vblanks (2026-11-11): only those seen cast are kept
    **{"spell %d" % s: dict(pad="cast", args=["--cheats", "spells,magic"], fx="spell=%d" % s,
                            frames=r(1900, 1923) + r(1940, 1963) + r(2020, 2031))
       for s in (3, 16)},
    # creatures' own effects: teleported, then put before one (KF4_FX=creature=ROW), --invincible, standing
    "fire elementals": dict(pad="stand", args=["--invincible", "--teleport", "1800:102:0"], fx="creature=67:700",
                            frames=r(2100, 2123) + r(2300, 2323)),
    "fire genie": dict(pad="stand", args=["--invincible", "--teleport", "1800:25:0"], fx="creature=74:800",
                       frames=r(2100, 2123) + r(2300, 2323)),
    "salamanders": dict(pad="stand", args=["--invincible", "--teleport", "1800:16:2"], fx="creature=16:900",
                        frames=r(2100, 2123) + r(2300, 2323)),
    "watchers": dict(pad="stand", args=["--invincible", "--teleport", "1800:22:0"], fx="creature=30:700",
                     frames=r(2100, 2123) + r(2300, 2323)),
    # under water and the MP drain, forced (KF4_FX), walking forward in the first map
    "underwater": dict(pad="walk_on", args=[], fx="water", frames=r(1800, 1823) + r(1900, 1911)),
    "drain": dict(pad="walk_on", args=[], fx="drainramp", frames=r(1800, 1823) + r(1900, 1911)),
}


def write_pad(path, case):
    with open(path, "w") as f:
        f.write(open(os.path.join(ROOT, "tests", "newgame.pad")).read() + "\n")
        if case["pad"]:
            f.write(open(os.path.join(ROOT, "tests", "motion", case["pad"] + ".pad")).read() + "\n")


def read_log(path):
    out = {}
    if not os.path.isfile(path):
        return out
    for line in open(path, errors="replace"):
        p = line.split(" ", 1)
        if p[0].isdigit() and " frame=" in line:
            # the state that is the same whoever draws: drop nothing but keep it short
            out[int(p[0])] = " ".join(x for x in p[1].split() if x.split("=")[0] in
                                      ("frame", "menu", "map", "pos", "heading", "hp"))
    return out


def game_frame(log, v):
    """the game's frame counter at vblank v (read_log's line), or None"""
    m = re.search(r"frame=(\d+)", log.get(v) or "")
    return int(m.group(1)) if m else None


def run(a, case, d, renderer, opts, frames, rate=None):
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "data"))
    pad = os.path.join(d, "case.pad")
    write_pad(pad, case)
    cmd = [a.exe, "--disc", a.disc, "--headless", "--no-audio", "--fast", "--memcard", "none", "--input", pad,
           "--frames", str(max(frames) + 2), "--renderer", renderer, "--dump-frames", ",".join(map(str, frames)),
           "--dump-dir", d, "--log", os.path.join(d, "log.txt")] + case["args"] + opts
    if rate:
        cmd += ["--frame-rate", str(rate)]
    env = dict(os.environ, KF4_DATA_DIR=os.path.join(d, "data"))
    env.pop("KF4_FX", None)
    if case["fx"]:
        env["KF4_FX"] = case["fx"]
    with open(os.path.join(d, "out.txt"), "w") as f:
        rc = subprocess.call(cmd, env=env, stdout=f, stderr=subprocess.STDOUT)
    shutil.rmtree(os.path.join(d, "data"), ignore_errors=True)
    return rc


def small(im, width=640):
    return im if im.width <= width else im.resize((width, round(im.height * width / im.width)), Image.BOX)


def slug(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def compare_case(a, name, case, profile):
    base = os.path.join(a.out, "runs", slug(name) + "_" + profile)
    ev = os.path.join(a.out, "evidence")
    os.makedirs(ev, exist_ok=True)
    frames = sorted(set(case["frames"]))
    res = {"case": name, "profile": profile, "frames": len(frames)}
    gs_d, pc_d = os.path.join(base, "gs"), os.path.join(base, "pc")
    res["gs_rc"] = run(a, case, gs_d, "gs", PROFILES[profile]["gs"], frames)
    res["pc_rc"] = run(a, case, pc_d, "pc", PROFILES[profile]["pc"], frames)
    gl, pl = read_log(os.path.join(gs_d, "log.txt")), read_log(os.path.join(pc_d, "log.txt"))
    rows = []
    for v in frames:
        name_v = "frame_%06d.png" % v
        pg, pp = os.path.join(gs_d, name_v), os.path.join(pc_d, name_v)
        row = {"vblank": v, "state": pl.get(v)}
        if gl.get(v) != pl.get(v):
            row["skip"] = "game state differs: gs %s / pc %s" % (gl.get(v), pl.get(v))
        elif not (os.path.isfile(pg) and os.path.isfile(pp)):
            row["skip"] = "picture missing (%s)" % ("gs" if not os.path.isfile(pg) else "pc")
        else:
            pc_im, gs_im = pictures_at_one_size(pp, pg)
            m, diff = measure(pc_im, gs_im, a.level)
            row.update(m)
        rows.append(row)
    done = [x for x in rows if "skip" not in x]
    res["compared"] = len(done)
    res["skipped"] = [x["vblank"] for x in rows if "skip" in x]
    res["skip_reasons"] = sorted({x["skip"].split(":")[0] for x in rows if "skip" in x})
    if done:
        res["blurred_max"] = max(x["blurred"] for x in done)
        res["blurred_median"] = float(np.median([x["blurred"] for x in done]))
        res["mean_max"] = max(x["mean"] for x in done)
        res["over_04"] = sum(1 for x in done if x["blurred"] > 0.004)
    worst = sorted(done, key=lambda x: (-x["blurred"], -x["mean"]))[:a.top]
    res["worst"] = []
    for x in worst:
        name_v = "frame_%06d.png" % x["vblank"]
        pc_im, gs_im = pictures_at_one_size(os.path.join(pc_d, name_v), os.path.join(gs_d, name_v))
        pc_im, gs_im = small(pc_im), small(gs_im)
        _, d = measure(pc_im, gs_im, a.level)
        out = os.path.join(ev, "%s_%s_v%06d.png" % (slug(name), profile, x["vblank"]))
        pair_picture(pc_im, gs_im, d, out)
        res["worst"].append(dict(x, pair=out))
    res["rows"] = rows

    if not a.no_240:
        d240 = os.path.join(base, "pc240")
        res["pc240_rc"] = run(a, case, d240, "pc", PROFILES[profile]["pc"], frames, rate=240)
        sheet = os.path.join(ev, "%s_%s_240_flagged.png" % (slug(name), profile))
        ff = subprocess.run([sys.executable, os.path.join(HERE, "flash_find.py"), d240, "--frames", "--sheet", sheet],
                            capture_output=True, text=True)
        text = ff.stdout + ff.stderr
        open(os.path.join(base, "flash_find_240.txt"), "w").write(text)
        m = re.search(r"(\d+) pictures, (\d+) the same as the one before, (\d+) flagged", text)
        res["pics240"], res["same240"], res["flagged240"] = (int(m.group(1)), int(m.group(2)), int(m.group(3))) \
            if m else (0, 0, -1)
        offs = [float(x) for x in re.findall(r"off\s+([\d.]+)%", text)]
        res["worst240"] = max(offs) if offs else 0.0
        res["flagged240_list"] = re.findall(r"^\s+(frame_\S+)\s+off", text, re.M)[:20]
        # the 240 run's game frames against the 30 run's pictures: the same game frame should look the same
        # (only the vblanks a new game frame begins at: at 30 the vblank after shows that frame again, at 240 a
        # picture between it and the next)
        p40 = read_log(os.path.join(d240, "log.txt"))
        differ, same_state = [], 0
        for v in frames:
            name_v = "frame_%06d.png" % v
            a_, b_ = os.path.join(d240, name_v), os.path.join(pc_d, name_v)
            if p40.get(v) != pl.get(v) or not (os.path.isfile(a_) and os.path.isfile(b_)):
                continue
            if game_frame(pl, v) is None or game_frame(pl, v) == game_frame(pl, v - 1):
                continue
            same_state += 1
            x, y = pictures_at_one_size(a_, b_)
            m2, _ = measure(x, y, a.level)
            if m2["blurred"] > 0.004:
                differ.append((v, m2["blurred"]))
        res["game240_checked"], res["game240_differ"] = same_state, differ
        if not a.keep:
            shutil.rmtree(d240, ignore_errors=True)
    if not a.keep:
        shutil.rmtree(gs_d, ignore_errors=True)
        shutil.rmtree(pc_d, ignore_errors=True)
    return res


def report(a, results):
    md = ["# PC renderer against paraLLEl-GS in motion", "",
          "Each vblank's pictures paired; `blurred` is the share of pixels over %d levels after a 3 x 3 blur (what "
          "is drawn differently), `>0.4%%` the pictures over 0.4%% of it. 240: pictures between flagged by "
          "flash_find --frames, and game frames at 240 that differ from the same vblank at 30." % a.level, "",
          "| case | profile | compared | not (why) | blurred max | median | >0.4% | 240 flagged (worst %) | "
          "240 game frames differ |", "|---|---|---|---|---|---|---|---|---|"]
    for x in results:
        md.append("| %s | %s | %d | %s | %s | %s | %s | %s | %s |" % (
            x["case"], x["profile"], x.get("compared", 0),
            ("%d (%s)" % (len(x["skipped"]), ", ".join(x["skip_reasons"]))) if x["skipped"] else "0",
            "%.2f%%" % (x["blurred_max"] * 100) if "blurred_max" in x else "",
            "%.2f%%" % (x["blurred_median"] * 100) if "blurred_median" in x else "",
            x.get("over_04", ""),
            ("%d of %d (%.2f)" % (x["flagged240"], x["pics240"], x["worst240"])) if "flagged240" in x else "",
            ("%d of %d" % (len(x["game240_differ"]), x["game240_checked"])) if "game240_differ" in x else ""))
    md += ["", "## The worst pictures", ""]
    for x in results:
        md.append("- %s, %s: %s" % (x["case"], x["profile"], ", ".join(
            "v%d %.2f%% (%s)" % (w["vblank"], w["blurred"] * 100, os.path.basename(w["pair"])) for w in x["worst"])))
        if x.get("flagged240_list"):
            md.append("  - 240 flagged: " + ", ".join(x["flagged240_list"][:8]))
        if x.get("game240_differ"):
            md.append("  - 240 game frames differing: " + ", ".join("v%d %.2f%%" % (v, b * 100)
                                                                     for v, b in x["game240_differ"][:8]))
    open(os.path.join(a.out, "report.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
    json.dump(results, open(os.path.join(a.out, "report.json"), "w"), indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="a scratch folder (outside the repository)")
    ap.add_argument("--exe", default=EXE)
    ap.add_argument("--disc", default=os.environ.get("KF4_DISC"), help="the disc image (or KF4_DISC)")
    ap.add_argument("--cases", help="comma-separated (default: all); 'list' lists them")
    ap.add_argument("--profiles", default="1x,4xw", help="1x, 4xw (default: both)")
    ap.add_argument("--no-240", action="store_true", help="leave out the 240 runs")
    ap.add_argument("--keep", action="store_true", help="keep the runs' pictures")
    ap.add_argument("--top", type=int, default=3, help="pairs kept a case and profile (3)")
    ap.add_argument("--level", type=int, default=24)
    a = ap.parse_args()
    if a.cases == "list":
        print("\n".join(CASES))
        return 0
    if not a.disc or not a.out:
        sys.exit("--out and --disc (or KF4_DISC) are needed")
    names = [n.strip() for n in a.cases.split(",")] if a.cases else list(CASES)
    for n in names:
        if n not in CASES:
            sys.exit("no case %r (--cases list)" % n)
    profiles = [p.strip() for p in a.profiles.split(",")]
    for p in profiles:
        if p not in PROFILES:
            sys.exit("no profile %r" % p)
    a.exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)
    results = []
    for n in names:
        for p in profiles:
            x = compare_case(a, n, CASES[n], p)
            results.append(x)
            print("%-16s %-4s compared %3d, not %2d, blurred max %6.2f%% median %5.2f%%, >0.4%% %3s%s" % (
                n, p, x.get("compared", 0), len(x["skipped"]), x.get("blurred_max", 0) * 100,
                x.get("blurred_median", 0) * 100, x.get("over_04", "-"),
                (", 240 flagged %d of %d, game frames differ %d of %d" % (
                    x["flagged240"], x["pics240"], len(x["game240_differ"]), x["game240_checked"]))
                if "flagged240" in x else ""), flush=True)
            report(a, results)
    print("motion_compare: %s" % os.path.join(a.out, "report.md"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
