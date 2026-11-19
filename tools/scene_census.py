#!/usr/bin/env python3
"""What each game frame hands the renderer, at a few scenes of play: the census behind the scene renderer's design
(docs/plans/scene-renderer-design.md, "The frame as a scene").

    tools/scene_census.py --out DIR [--disc ISO] [--scenarios start,pond,...] [--report-only]

Each scenario (tools/layer_inventory.py's) is played headless at 30 game frames a second with native draw on, at the
owner's scale (2x with native widescreen), with the PC renderer's frame census (KF4_PC_FRAME_CENSUS: pcr_frame_census.h)
at the frames around the scenario's moments, and its overdraw counter (KF4_PC_OVERDRAW: DATE's fragments). Then
census.md: each scene's median frame (native draws by kind and program, GIF primitives by path and class, the draws in
order and their runs, transfers), and census.json.

Raw output holds the game's data: --out must be outside the repository.
"""
import argparse
import collections
import json
import os
import statistics
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import layer_inventory as inv  # noqa: E402
import map_tour  # noqa: E402

# name -> (layer_inventory's scenario, the vblanks counted around, what it is)
SCENES = {
    "start": ("start", [1790, 1806], "099's start, standing"),
    "pond": ("pond", [1990, 2110], "009, the swamp: the reflection"),
    "palace": ("palace", [2090, 2310], "022, the Queen's Palace, a Watcher"),
    "combat": ("combat", [2500, 2660], "002, a Maneater fought with the club"),
    "water": ("canal water", [1990, 2010], "029, under water (forced)"),
    "menus": ("menus", [1900, 1920], "the menu's pages"),
    "title": ("title", [400, 530], "the title and Press Start"),
}


# The scenes outside play: their loops' pictures carry no vblank of the game (the census numbers them 0), so every
# frame is counted, and those without native draws (the loop's, not play's) are summed
LOOPS = ("menus", "title")


def run_scene(a, name):
    scen, (v0, v1), what = SCENES[name]
    sc = inv.scenario(scen)
    folder = os.path.join(a.out, name)
    census = os.path.join(folder, "census.jsonl")
    if os.path.exists(census) and os.path.getsize(census) > 0 and not a.force:
        return census
    os.makedirs(folder, exist_ok=True)
    pad = os.path.join(folder, "input.pad")
    inv.write_pad(pad, sc, None)
    data_dir = os.path.join(folder, "data")
    os.makedirs(data_dir, exist_ok=True)
    os.makedirs(os.path.join(folder, "pictures"), exist_ok=True)
    end = v1 + 4
    cmd = [a.exe, "--disc", a.disc, "--headless", "--fast", "--no-audio", "--saves", "none", "--clock", inv.CLOCK,
           "--input", pad, "--frames", str(end), "--stats", "--native-draw", "on", "--frame-rate", "30",
           "--widescreen", "--native-scale", "2", "--native-widescreen", "--antialias", "fxaa",
           "--dump-frames", "%d,%d" % (v0, v1), "--dump-dir", os.path.join(folder, "pictures")]
    cmd += inv.game_args(a, sc, folder)
    env = dict(os.environ, KF4_DATA_DIR=data_dir, KF4_PC_FRAME_CENSUS=census,
               KF4_PC_OVERDRAW=os.path.join(folder, "overdraw.txt"))
    if name not in LOOPS:
        env["KF4_PC_FRAME_CENSUS_AT"] = "%d-%d" % (v0, v1)
    env.pop("KF4_FX", None)
    if sc["fx"]:
        env["KF4_FX"] = sc["fx"]
    env.update(sc["env"])
    with open(os.path.join(folder, "stderr.txt"), "w") as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=1800)
    with open(os.path.join(folder, "cmd.json"), "w") as f:
        json.dump({"cmd": cmd, "fx": sc["fx"], "env": sc["env"]}, f, indent=1)
    return census


def read(path):
    out = []
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


def date_fragments(folder, v0, v1):
    """KF4_PC_OVERDRAW's lines: native DATE batches' fragment shader invocations a vblank (date column 1)"""
    path = os.path.join(folder, "overdraw.txt")
    by = collections.defaultdict(lambda: [0, 0, 0])  # vblank -> date batches, date invocations, all invocations
    if not os.path.exists(path):
        return by
    for line in open(path):
        p = line.split()
        if not p or p[0] != "Q":
            continue
        v = int(p[1])
        if not (v0 <= v <= v1):
            continue
        date, batches, inv_ = int(p[5]), int(p[9]), int(p[10])
        if date:
            by[v][0] += batches
            by[v][1] += inv_
        by[v][2] += inv_
    return by


def med(xs):
    return statistics.median(xs) if xs else 0


def summarize(a):
    report = {}
    for name in a.names:
        scen, (v0, v1), what = SCENES[name]
        folder = os.path.join(a.out, name)
        frames = read(os.path.join(folder, "census.jsonl"))
        # (the frames with the game's 3D: those with native draws, else all of them)
        with3d = [f for f in frames if sum(f["native"]["draws"].values()) > 0]
        use = with3d or frames
        if name in LOOPS:
            use = [f for f in frames if sum(f["native"]["draws"].values()) == 0 and f["vblank"] == 0]
        if not use:
            continue
        r = {"what": what, "frames": len(use)}
        r["bytes"] = [med([f["bytes"][i] for f in use]) for i in range(4)]
        nd = collections.defaultdict(list)
        for key in ("draws", "batches", "triangles", "sphere"):
            keys = set()
            for f in use:
                keys |= set(f["native"][key])
            r["nd_" + key] = {k: med([f["native"][key].get(k, 0) for f in use]) for k in sorted(keys)}
        r["meshes"] = med([f["native"]["meshes"] for f in use])
        r["cameras"] = med([f["native"]["cameras"] for f in use])
        gk = set()
        for f in use:
            gk |= set(f["gif"])
        r["gif"] = {k: [med([f["gif"].get(k, [0, 0])[0] for f in use]), med([f["gif"].get(k, [0, 0])[1] for f in use])]
                    for k in sorted(gk)}
        r["order"] = {k: med([f["order"][k] for f in use]) for k in
                      ("draws", "gif_runs", "pipeline_runs", "pipeline_texture_runs", "target_runs", "pipelines",
                       "textures")}
        tg = collections.Counter()
        zb = collections.Counter()
        for f in use:
            tg.update(f["order"]["targets"])
            zb.update(f["order"]["zbufs"])
        r["targets"] = sorted(tg)
        r["zbufs"] = sorted(zb)
        npc = set()
        for f in use:
            npc |= set(f["order"]["native_pipeline_changes"])
        r["native_pipeline_changes"] = {k: med([f["order"]["native_pipeline_changes"].get(k, 0) for f in use])
                                        for k in sorted(npc)}
        r["p280"] = {"batches": med([f["p280"]["batches"] for f in use]), "runs": med([f["p280"]["runs"] for f in use])}
        ck = set()
        for f in use:
            ck |= set(f["p280"]["changes"])
        r["p280"]["changes"] = {k: med([f["p280"]["changes"].get(k, 0) for f in use]) for k in sorted(ck)}
        r["transfers"] = {k: med([f["transfers"][k] for f in use]) for k in
                          ("uploads", "upload_bytes", "again", "copies", "read_backs")}
        tk = set()
        for f in use:
            tk |= set(f["transfers"]["to"])
        r["upload_to"] = {k: [med([f["transfers"]["to"].get(k, [0, 0])[0] for f in use]),
                              med([f["transfers"]["to"].get(k, [0, 0])[1] for f in use])] for k in sorted(tk)}
        cps = collections.Counter()
        for f in use:
            cps.update(f["transfers"]["copy_kinds"])
        r["copy_kinds"] = {k: v / len(use) for k, v in cps.items()}
        if all("canvas" in f for f in use):
            r["canvas"] = {k: med([f["canvas"][k] for f in use]) for k in use[0]["canvas"]}
        dates = date_fragments(folder, v0, v1)
        if dates:
            vals = list(dates.values())
            r["date_batches"] = med([d[0] for d in vals])
            r["date_fragments"] = med([d[1] for d in vals])
            r["native_fragments"] = med([d[2] for d in vals])
        report[name] = r
    with open(os.path.join(a.out, "census.json"), "w") as f:
        json.dump(report, f, indent=1)
    write_md(a, report)


def write_md(a, report):
    L = ["# The frame census (tools/scene_census.py)", "",
         "Each scene's median game frame (headless, 30 game frames a second, native draw on, 2x with native "
         "widescreen). Frames: those counted with native draws (the 3D), else all.", ""]
    names = list(report)
    def row(label, f):
        L.append("| %s | %s |" % (label, " | ".join(str(f(report[n])) for n in names)))
    L.append("| | " + " | ".join(names) + " |")
    L.append("|---|" + "---|" * len(names))
    row("frames counted", lambda r: r["frames"])
    row("bytes PATH1 / 2 / 3", lambda r: "%d / %d / %d" % tuple(r["bytes"][1:4]))
    row("native draws", lambda r: int(sum(v for k, v in r["nd_draws"].items() if not k.startswith("pass"))))
    row("native passes", lambda r: int(sum(v for k, v in r["nd_draws"].items() if k.startswith("pass"))))
    row("native batches (VU1 runs)", lambda r: int(sum(r["nd_batches"].values())))
    row("native triangles", lambda r: int(sum(r["nd_triangles"].values())))
    row("meshes, cameras", lambda r: "%d, %d" % (r["meshes"], r["cameras"]))
    row("GIF primitives", lambda r: int(sum(v[0] for v in r["gif"].values())))
    row("draws in order (GIF runs + native batches)", lambda r: int(r["order"]["draws"]))
    row("runs of one pipeline", lambda r: int(r["order"]["pipeline_runs"]))
    row("runs of one pipeline and texture", lambda r: int(r["order"]["pipeline_texture_runs"]))
    row("runs of one target (passes)", lambda r: int(r["order"]["target_runs"]))
    row("pipelines, textures", lambda r: "%d, %d" % (r["order"]["pipelines"], r["order"]["textures"]))
    row("targets", lambda r: "; ".join(r["targets"]))
    row("Z buffers", lambda r: "; ".join(r["zbufs"]))
    row("0x280 batches, runs", lambda r: "%d, %d" % (r["p280"]["batches"], r["p280"]["runs"]))
    row("uploads (MB)", lambda r: "%d (%.2f)" % (r["transfers"]["uploads"], r["transfers"]["upload_bytes"] / 1e6))
    row("uploads asked again, copies, read-backs", lambda r: "%d, %d, %d" % (
        r["transfers"]["again"], r["transfers"]["copies"], r["transfers"]["read_backs"]))
    row("DATE: native batches, fragments", lambda r: "%s, %s" % (r.get("date_batches", "-"), r.get("date_fragments", "-")))
    row("canvas: batches, draw calls, passes", lambda r: "%d, %d, %d" % (
        r["canvas"]["batches"], r["canvas"]["native_draw_calls"], r["canvas"]["passes"]) if "canvas" in r else "-")
    L.append("")
    for n in names:
        r = report[n]
        L += ["## %s: %s" % (n, r["what"]), ""]
        L.append("Native draws (draws / batches / triangles / sphere passes):")
        L.append("")
        for k in r["nd_draws"]:
            L.append("- %s: %d / %d / %d / %d" % (k, r["nd_draws"][k], r["nd_batches"].get(k, 0),
                                                   r["nd_triangles"].get(k, 0), r["nd_sphere"].get(k, 0)))
        L += ["", "GIF primitives (count / pixels, GS pixels, a triangle's half its rectangle):", ""]
        for k, (c, px) in r["gif"].items():
            L.append("- %s: %d / %d" % (k, c, px))
        L += ["", "Native batches and pipeline changes: %s" % json.dumps(r["native_pipeline_changes"]),
              "", "0x280: %s" % json.dumps(r["p280"]),
              "", "Uploads by destination (count, bytes): %s" % json.dumps(r["upload_to"]),
              "", "Copies a frame: %s" % json.dumps(r["copy_kinds"]), ""]
        if "canvas" in r:
            L += ["Canvas: %s" % json.dumps(r["canvas"]), ""]
    with open(os.path.join(a.out, "census.md"), "w") as f:
        f.write("\n".join(L) + "\n")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True)
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--exe")
    p.add_argument("--scenarios", default=",".join(SCENES))
    p.add_argument("--report-only", action="store_true")
    p.add_argument("--force", action="store_true")
    a = p.parse_args()
    a.out = os.path.abspath(a.out)
    if a.out == ROOT or a.out.startswith(ROOT + os.sep):
        sys.exit("scene_census: --out must be outside the repository")
    os.makedirs(a.out, exist_ok=True)
    a.names = [s.strip() for s in a.scenarios.split(",") if s.strip()]
    if not a.report_only:
        if not a.disc:
            sys.exit("scene_census: pass --disc or set KF4_DISC")
        a.exe = a.exe or map_tour.find_exe("kf4")
        for n in a.names:
            print("scene_census: %s" % n, flush=True)
            run_scene(a, n)
    summarize(a)
    print(open(os.path.join(a.out, "census.md")).read().split("\n## ")[0])


if __name__ == "__main__":
    main()
