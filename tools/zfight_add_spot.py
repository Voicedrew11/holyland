#!/usr/bin/env python3
"""Add a z-fighting spot (tools/zfight_test.py, docs/port/ZFIGHT_TEST.md) from where you stand in the game.

    tools/zfight_add_spot.py NAME [--quick FILE] [--about TEXT] [--spots DIR] [--exe KF4] [--disc ISO]
                             [--motion turn,step,side,walk] [--force] [--no-run]

In the game, stand where the surfaces fight, face them and press F2 (quick save). Then this:
1. reads the newest quick save in the data folder's quicksaves/ (or --quick FILE) for the map, the player's
   position, heading and pitch only; the save itself is not copied anywhere and nothing in the data folder is
   written;
2. writes NAME.json into the spots folder (default tests/zspots in this repository, or KF4_ZSPOTS): no game data,
   so it can be committed;
3. checks that the spot built from those numbers (a new game's save, moved there) shows the same picture as your
   quick save (game progress, open doors, broken things, can make them differ: it says so), and
4. runs the test on the spot once and writes the score into the JSON as "found" (the build that showed the fight
   should fail it (a score over 100%); under 100% the test does not see this fight: try --motion, a bigger
   "step", or tell a developer).
"""
import argparse
import datetime
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import zfight_test as zt  # noqa: E402


def owner_data_dir():
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA", ""), "holyland")
    return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "holyland")


def newest_quick_save():
    files = glob.glob(os.path.join(owner_data_dir(), "quicksaves", "*.kf4quick"))
    if not files:
        sys.exit("zfight_add_spot: no quick save in %s (press F2 in the game first)" % os.path.join(
            owner_data_dir(), "quicksaves"))
    return max(files, key=os.path.getmtime)


def one_picture(exe, disc, save, folder, spot):
    """The picture after loading save, as the test takes its views"""
    os.makedirs(folder)
    pad = os.path.join(folder, "p.pad")
    with open(pad, "w") as f:
        f.write("100 CROSS 4\n")
    shot = zt.FIRST_LOAD + int(spot["settle"])
    cmd = zt.compatible(exe, [exe, "--disc", disc, "--headless", "--no-audio", "--fast", "--frames", str(shot + 2),
                              "--clock", zt.CLOCK, "--saves", "none", "--input", pad, "--quick-load", save,
                              "--dump-frames", str(shot), "--dump-dir", folder, "--native-scale", str(spot["scale"]),
                              "--frame-rate", str(spot["frame_rate"])] + zt.OWNER + list(spot["args"]))
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   env=dict(os.environ, KF4_DATA_DIR=os.path.join(folder, "data")))
    path = os.path.join(folder, "frame_%06d.png" % shot)
    return zt.read(path) if os.path.exists(path) else None


def compare_with_save(exe, disc, quick, spot, work):
    """How far the spot made from numbers is from the player's own quick save: the share of pixels (outside the
    HUD) that differ by more than 16 levels"""
    base = zt.read_quick(zt.make_base(exe, disc, os.path.join(work, "base")))
    made = os.path.join(work, "made.kf4quick")
    zt.write_made_up(base, made, spot["map"], spot["position"], spot["heading"], spot["pitch"])
    own = os.path.join(work, "own.kf4quick")
    shutil.copy(quick, own)  # (a copy in the work folder, deleted after; the data folder is only read)
    a = one_picture(exe, disc, made, os.path.join(work, "made"), spot)
    b = one_picture(exe, disc, own, os.path.join(work, "own"), spot)
    if a is None or b is None:
        return None
    region = zt.counted_region(a.shape[:2], spot)
    d = np.abs(a - b).max(axis=2)[region]
    return round(100.0 * float((d > 16).mean()), 2)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name")
    parser.add_argument("--quick", help="the quick save to read (default: the newest in the data folder)")
    parser.add_argument("--about", default="", help="what fights there (a line)")
    parser.add_argument("--spots", default=os.environ.get("KF4_ZSPOTS", zt.DEFAULT_SPOTS))
    parser.add_argument("--exe", default=os.environ.get("KF4_EXE"))
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--motion", help="the motions to test (default all)")
    parser.add_argument("--force", action="store_true", help="replace a spot of that name")
    parser.add_argument("--no-run", action="store_true", help="only write the JSON")
    args = parser.parse_args()
    name = args.name.strip()
    if not name or any(c in name for c in "\\/:*?\"<>| "):
        sys.exit("zfight_add_spot: a spot's name is a plain word (letters, digits, - and _)")
    path = os.path.join(args.spots, name + ".json")
    if os.path.exists(path) and not args.force:
        sys.exit("zfight_add_spot: %s exists (--force replaces it)" % path)
    quick = args.quick or newest_quick_save()
    place = zt.quick_place(quick)
    spot = {"about": args.about or "added %s from a quick save" % datetime.date.today().isoformat(),
            "map": place["map"], "position": [round(v, 3) for v in place["position"]],
            "heading": round(place["heading"], 5), "pitch": round(place["pitch"], 5)}
    if args.motion:
        spot["motion"] = args.motion.split(",") if "," in args.motion else args.motion
    os.makedirs(args.spots, exist_ok=True)
    with open(path, "w") as f:
        json.dump(spot, f, indent=2)
        f.write("\n")
    print("zfight_add_spot: %s: map %d at %.1f, %.1f, %.1f, heading %.4f, pitch %.4f (from %s)" % (
        path, place["map"], *place["position"], place["heading"], place["pitch"], quick))
    if args.no_run:
        return
    if not args.disc:
        sys.exit("zfight_add_spot: pass --disc or set KF4_DISC to check and run the spot")
    exe = os.path.abspath(args.exe or zt.find_exe())
    work = tempfile.mkdtemp(prefix="kf4_zspot_")
    try:
        loaded = zt.load_spot(path)
        differ = compare_with_save(exe, args.disc, quick, loaded, work)
        if differ is None:
            print("zfight_add_spot: could not take the pictures to compare with the quick save")
        elif differ > 1.0:
            print("zfight_add_spot: WARNING: the spot made from numbers differs from your quick save at %.1f%% of the "
                  "picture (game progress: a door, a broken wall, a creature?). Look at %s and %s" % (
                      differ, os.path.join(work, "made"), os.path.join(work, "own")))
        else:
            print("zfight_add_spot: the spot shows what your quick save shows (%.2f%% of pixels differ)" % differ)
        out = os.path.join(work, "test")
        r = subprocess.run([sys.executable, os.path.join(HERE, "zfight_test.py"), "--spots", args.spots, "--spot",
                            name, "--exe", exe, "--disc", args.disc, "--out", out])
        with open(os.path.join(out, name, "score.json")) as f:
            result = json.load(f)
        with open(path) as f:
            raw = json.load(f)
        raw["found"] = {"score": result.get("score"), "motions": {k: m["score"] for k, m in
                                                                  result.get("motions", {}).items()},
                        "exe": exe, "date": datetime.date.today().isoformat(), "same_as_save_pct": differ}
        with open(path, "w") as f:
            json.dump(raw, f, indent=2)
            f.write("\n")
        evidence = "(none)"
        if os.path.exists(os.path.join(out, name, "evidence.png")):
            # (outside the spots folder: the picture is the game's)
            ev_dir = os.path.join(tempfile.gettempdir(), "kf4_zspots_evidence")
            os.makedirs(ev_dir, exist_ok=True)
            evidence = os.path.join(ev_dir, name + ".png")
            shutil.copy(os.path.join(out, name, "evidence.png"), evidence)
        print("zfight_add_spot: %s scored %s%% of its limit: %s. Evidence: %s" % (
            name, result.get("score"),
            "the test sees it" if r.returncode else "under the limit: the test does not see a fight here with "
                                                     "this build", evidence))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
