#!/usr/bin/env python3
"""The scene renderer (--renderer scene, docs/plans/scene-renderer-design.md) against the PC renderer in the full-screen
layers' scenarios (tools/layer_inventory.py's: the title, the menus, the save screen, the map viewers, the map change,
under water, the Dark Succubus' copy, combat, the spells ...): each played headless by both, with native draw on, and
their pictures at the scenario's moments compared byte for byte.

    tools/scene_scenarios.py --out DIR [--disc ISO] [--exe EXE] [--scenarios NAME,...|all] [--jobs N]
                             [--reference ENV | --reference-exe EXE] [--scale 2] [--keep] [--gs]

Each scenario as layer_inventory.py's capture step plays it (its pad script, teleport, KF4_FX, options; 2x with
native widescreen), but native draw on (off where the scenario's settings say so: "start native off") and no GS
capture: once with --renderer pc and the reference's environment
(default KF4_PC_DEPTH=unorm: the depth the scene renderer has), once with --renderer scene, each with --stats and
--dump-frames at the scenario's moments (the last vblank of each of its captures). Then for each moment: both
pictures there, and how many pixels differ (and by more than 24 levels). A scenario passes when the scene renderer
ran to its end and drew every moment the PC renderer drew.

With --reference-exe EXE the reference is that build's scene renderer instead (an earlier slice's: "pictures byte for
byte slice 1's"), still in NAME/pc/.

With --gs (meant for --scale 1), a third run by paraLLEl-GS (--renderer gs), and at each moment the scene and PC
renderers each measured against it too, all three without widescreen: pixels differing (over 24) and the mean error over the pixels where scene
and pc differ (where one of them is paraLLEl-GS's and the other not, the one that is).

Into DIR: NAME/{pc,scene[,gs]}/ (pictures, stderr.txt with the [stats]), report.md and report.json. The pictures hold the
game's data: DIR outside the repository. Needs Pillow and numpy.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import layer_inventory as inv  # noqa: E402
import map_tour  # noqa: E402

EXE = os.path.join(ROOT, "out", "linux-clang", "kf4")


def play(a, name, renderer, env_extra, folder_name=None, exe=None):
    sc = inv.scenario(name)
    folder = os.path.join(a.out, inv.slug(name), folder_name or renderer)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(os.path.join(folder, "pictures"))
    pad = os.path.join(folder, "input.pad")
    inv.write_pad(pad, sc, None)
    data_dir = os.path.join(folder, "data")
    os.makedirs(data_dir)
    end = inv.last_vblank(sc)
    moments = [v + inv.CAPTURE_VBLANKS - 1 for v in sc["captures"]]
    cmd = [exe or a.exe, "--disc", a.disc, "--headless", "--fast", "--no-audio", "--saves", "none", "--clock", inv.CLOCK,
           "--input", pad, "--frames", str(end), "--stats", "--native-scale", str(a.scale),
           "--antialias", "off", "--native-draw", sc["settings"].get("native_draw", "on"),
           "--frame-rate", "30",
           "--renderer", renderer, "--dump-frames", ",".join(map(str, moments)),
           "--dump-dir", os.path.join(folder, "pictures")]
    # (paraLLEl-GS draws the GS's 640 wide: with --gs, no widescreen, so that the pictures pair)
    cmd += ["--no-native-widescreen"] if a.gs else ["--widescreen", "--native-widescreen"]
    cmd += inv.game_args(a, sc, folder)
    env = dict(os.environ, KF4_DATA_DIR=data_dir)
    env.pop("KF4_FX", None)
    if sc["fx"]:
        env["KF4_FX"] = sc["fx"]
    env.update(sc["env"])
    env.update(env_extra)
    rc, wall = inv.run(cmd, env, os.path.join(folder, "stderr.txt"), timeout=900)
    shutil.rmtree(data_dir, ignore_errors=True)
    return {"exit": rc, "wall": round(wall, 1), "moments": moments}


def compare(a, name, moments):
    out = []
    for v in moments:
        f = "frame_%06d.png" % v
        renderers = ("pc", "scene", "gs") if a.gs else ("pc", "scene")
        p = [os.path.join(a.out, inv.slug(name), r, "pictures", f) for r in renderers]
        have = [os.path.exists(x) for x in p]
        row = {"vblank": v, "pc": have[0], "scene": have[1]}
        if all(have[:2]):
            x = np.asarray(Image.open(p[0]).convert("RGB"), dtype=np.int16)
            y = np.asarray(Image.open(p[1]).convert("RGB"), dtype=np.int16)
            if x.shape != y.shape:
                row["size"] = [list(x.shape), list(y.shape)]
            else:
                d = np.abs(x - y).max(2)
                row["differ"] = int((d > 0).sum())
                row["over24"] = int((d > 24).sum())
                if a.gs and have[2]:
                    g = np.asarray(Image.open(p[2]).convert("RGB"), dtype=np.int16)
                    if g.shape == x.shape:
                        where = d > 0
                        for r, im in (("pc", x), ("scene", y)):
                            e = np.abs(im - g).max(2)
                            row[r + "_gs"] = {"differ": int((e > 0).sum()), "over24": int((e > 24).sum()),
                                              "mean_where_differ": round(float(e[where].mean()), 2) if where.any()
                                              else 0.0}
        out.append(row)
    return out


def scenario_job(a, name):
    ref_env = dict(kv.split("=", 1) for kv in a.reference.split(",") if kv)
    # (the reference: the PC renderer, or with --reference-exe that build's scene renderer, in pc/ all the same)
    pc = play(a, name, "scene" if a.reference_exe else "pc", ref_env, "pc", a.reference_exe)
    scene = play(a, name, "scene", {})
    if a.gs:
        play(a, name, "gs", {})
    rows = compare(a, name, pc["moments"])
    drawn = all(r["scene"] or not r["pc"] for r in rows)
    result = {"name": name, "pc": pc, "scene": scene, "moments": rows,
              "passes": scene["exit"] == 0 and drawn,
              "identical": all(r.get("differ") == 0 for r in rows if r["pc"] and r["scene"])}
    if not a.keep:
        for r in ("pc", "scene", "gs"):
            shutil.rmtree(os.path.join(a.out, inv.slug(name), r, "pictures"), ignore_errors=True)
    return result


def report(a, results):
    what = ("the scene renderer of %s" % a.reference_exe) if a.reference_exe else \
        ("the PC renderer (%s)" % (a.reference or "its default"))
    md = ["# The scene renderer against %s, the layer inventory's scenarios" % what,
          "", "Made by `tools/scene_scenarios.py`: headless, 30, native draw on, %dx%s, no "
          "anti-aliasing; pictures at each scenario's moments." % (a.scale, "" if a.gs else " with native widescreen"), "",
          "| scenario | scene exit | pc exit | moments drawn (scene / pc) | identical | pixels differing (over 24) |",
          "|---|---|---|---|---|---|"]
    for r in results:
        rows = r["moments"]
        md.append("| %s | %d | %d | %d / %d | %s | %s |" % (
            r["name"], r["scene"]["exit"], r["pc"]["exit"], sum(x["scene"] for x in rows), sum(x["pc"] for x in rows),
            "yes" if r["identical"] else "no",
            ", ".join("%d (%d)" % (x.get("differ", -1), x.get("over24", -1)) for x in rows)))
    if a.gs:
        md += ["", "Against paraLLEl-GS: pixels differing (over 24); mean error where scene and pc differ.", "",
               "| scenario | vblank | pc | scene | pc mean | scene mean |", "|---|---|---|---|---|---|"]
        for r in results:
            for x in r["moments"]:
                if "pc_gs" in x:
                    pg, sg = x["pc_gs"], x["scene_gs"]
                    md.append("| %s | %d | %d (%d) | %d (%d) | %.2f | %.2f |" % (
                        r["name"], x["vblank"], pg["differ"], pg["over24"], sg["differ"], sg["over24"],
                        pg["mean_where_differ"], sg["mean_where_differ"]))
    passed = sum(r["passes"] for r in results)
    same = sum(r["identical"] for r in results)
    md += ["", "%d of %d scenarios drawn to their end by the scene renderer; %d byte for byte the PC renderer's." %
           (passed, len(results), same)]
    open(os.path.join(a.out, "report.md"), "w").write("\n".join(md) + "\n")
    json.dump(results, open(os.path.join(a.out, "report.json"), "w"), indent=1)
    print("\n".join(md))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True)
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--exe", default=EXE)
    p.add_argument("--gsreplay", default=None)
    p.add_argument("--scenarios", default="all")
    p.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 4))
    p.add_argument("--reference", default="KF4_PC_DEPTH=unorm",
                   help="the PC renderer's environment, NAME=VALUE,... (default KF4_PC_DEPTH=unorm)")
    p.add_argument("--reference-exe", default=None,
                   help="the reference is this build's scene renderer (e.g. an earlier slice's), not the PC renderer")
    p.add_argument("--scale", type=int, default=2)
    p.add_argument("--keep", action="store_true", help="keep the pictures")
    p.add_argument("--gs", action="store_true", help="paraLLEl-GS played too, both measured against it (use --scale 1)")
    a = p.parse_args()
    if not a.disc:
        sys.exit("--disc ISO (or KF4_DISC)")
    a.out = os.path.abspath(a.out)
    os.makedirs(a.out, exist_ok=True)
    a.force = True
    a.disc_files = map_tour.Disc(a.disc)
    a.base = None  # (the scenarios start with a New Game: no made-up save)
    names = list(inv.SCENARIOS) if a.scenarios in ("", "all") else [s.strip() for s in a.scenarios.split(",")]
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        results = list(pool.map(lambda n: scenario_job(a, n), names))
    report(a, results)


if __name__ == "__main__":
    main()
