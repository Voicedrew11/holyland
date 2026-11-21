#!/usr/bin/env python3
"""The scene renderer's frame held back (docs/plans/scene-slice4-results.md, "4b") against the frame drawn as its scene
closes: every motion case of tools/motion_compare.py played twice by the scene renderer, KF4_SCN_HOLD_FRAME=1 and 0,
and their pictures compared byte for byte (held back, the pictures must not change).

    tools/hold_compare.py --out DIR [--exe EXE] [--disc ISO] [--cases a,b] [--rates 60,240] [--profiles 1x,2xw]
                          [--jobs N] [--env K=V,...]

KF4_INTERP_T0=1 (frame_interp.c's): each game frame's own vblank drawn as a picture between at t 0, as with the mouse
turned, so that the frame is taken at its own vblank and held back under a pad script that repeats (the mouse's own
turns go by the wall clock). --env adds variables to both runs (KF4_INTERP_T0=0: the frames taken as without the mouse).
Into DIR: report.md (a line a case, profile and rate: pictures, those that differ, the held runs' --stats line). The
pictures hold the game's data: keep DIR outside the repository.
"""
import argparse
import concurrent.futures
import filecmp
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import motion_compare as mc  # noqa: E402

PROFILES = {
    "1x": ["--native-scale", "1", "--no-native-widescreen", "--native-draw", "on"],
    "2xw": ["--native-scale", "2", "--widescreen", "--native-widescreen", "--native-draw", "on"],
}


def play(a, name, case, profile, rate, hold):
    """motion_compare.run's run of the scene renderer, with the hold's variables"""
    d = os.path.join(a.out, "runs", "%s_%s_%d_hold%d" % (mc.slug(name), profile, rate, hold))
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "data"))
    pad = os.path.join(d, "case.pad")
    mc.write_pad(pad, case)
    frames = case["frames"]
    cmd = [a.exe, "--disc", a.disc, "--headless", "--no-audio", "--fast", "--saves", "none", "--input", pad,
           "--frames", str(max(frames) + 2), "--renderer", "scene", "--dump-frames", ",".join(map(str, frames)),
           "--dump-dir", d, "--log", os.path.join(d, "log.txt"), "--stats", "--frame-rate", str(rate)] + \
        case["args"] + PROFILES[profile]
    env = dict(os.environ, KF4_DATA_DIR=os.path.join(d, "data"), KF4_SCN_HOLD_FRAME=str(hold), **a.env)
    env.pop("KF4_FX", None)
    if case["fx"]:
        env["KF4_FX"] = case["fx"]
    with open(os.path.join(d, "out.txt"), "w") as f:
        rc = subprocess.call(cmd, env=env, stdout=f, stderr=subprocess.STDOUT)
    shutil.rmtree(os.path.join(d, "data"), ignore_errors=True)
    return d, rc


def compare(a, name, case, profile, rate):
    runs = {}
    for hold in (1, 0):
        runs[hold] = play(a, name, case, profile, rate, hold)
    pictures = sorted(f for f in os.listdir(runs[0][0]) if f.endswith(".png"))
    differ = [f for f in pictures if not os.path.exists(os.path.join(runs[1][0], f)) or
              not filecmp.cmp(os.path.join(runs[0][0], f), os.path.join(runs[1][0], f), shallow=False)]
    held = ""
    out = os.path.join(runs[1][0], "out.txt")
    if os.path.exists(out):
        for line in open(out, errors="replace"):
            if "frames held back" in line:
                held = re.sub(r"^\[stats\] scene renderer ", "", line.strip())
    return dict(case=name, profile=profile, rate=rate, pictures=len(pictures), differ=len(differ),
                first=differ[:5], rc=(runs[1][1], runs[0][1]), held=held)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--exe", default=mc.EXE)
    ap.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    ap.add_argument("--cases")
    ap.add_argument("--rates", default="60,240")
    ap.add_argument("--profiles", default="1x,2xw")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--env", default="KF4_INTERP_T0=1")
    a = ap.parse_args()
    if not a.disc:
        sys.exit("hold_compare: pass --disc or set KF4_DISC")
    a.out = os.path.abspath(a.out)
    if a.out == ROOT or a.out.startswith(ROOT + os.sep):
        sys.exit("hold_compare: --out must be outside the repository")
    os.makedirs(a.out, exist_ok=True)
    a.env = dict(kv.split("=", 1) for kv in a.env.split(",") if kv)
    names = [c.strip() for c in a.cases.split(",")] if a.cases else list(mc.CASES)
    jobs = [(n, p, int(r)) for n in names for p in a.profiles.split(",") for r in a.rates.split(",")]
    results = []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        futures = {pool.submit(compare, a, n, mc.CASES[n], p, r): (n, p, r) for n, p, r in jobs}
        for f in concurrent.futures.as_completed(futures):
            res = f.result()
            results.append(res)
            print("%-16s %-4s %3d: %d of %d pictures differ%s" % (
                res["case"], res["profile"], res["rate"], res["differ"], res["pictures"],
                " (" + ", ".join(res["first"]) + ")" if res["differ"] else ""), flush=True)
    results.sort(key=lambda x: (names.index(x["case"]), x["profile"], x["rate"]))
    with open(os.path.join(a.out, "report.md"), "w") as f:
        f.write("| Case | Profile | Rate | Pictures | Differ | Held (the held run's --stats) |\n|---|---|---|---|---|---|\n")
        for res in results:
            f.write("| %s | %s | %d | %d | %d | %s |\n" % (res["case"], res["profile"], res["rate"], res["pictures"],
                                                         res["differ"], res["held"] or "none"))
    print("differ in all: %d of %d" % (sum(x["differ"] for x in results), sum(x["pictures"] for x in results)))


if __name__ == "__main__":
    main()
