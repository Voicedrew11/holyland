#!/usr/bin/env python3
"""What the world costs on the GPU drawn the GS's way and with the GS-exact mechanisms switched off
(docs/plans/world-plain.md).

    tools/world_plain.py --out DIR [--disc ISO] [--scenes start,pond,palace] [--variants exact,gpu,...|list]
                         [--scales 1,2,4] [--rates 30] [--rounds N] [--report-only]

Each scene of tools/layer_inventory.py played in a real window with the owner's settings (its live step: a copy of
the data folder, KF4_READ_SETTINGS=1, KF4_GPU_DETAIL=1), once for each variant (developers' KF4_PC_* switches, off the
exact path: for timing only) at each native scale. The rounds run the whole matrix again, in the same order, so a
drift of the GPU's clocks shows as a spread rather than as a variant's cost.

The world: the native draws (VU1 programs 0x000, 0x280 the skinned models and 0x700), less program 0x000's entity
pool DATE draws and the sky (read from a target's copy), with exact Z's planes (`pc planes`). Every figure is GPU ms a game
frame (the run's GPU time over its game frames; at 30 a game frame is drawn once).

Raw output (profiles, logs) is the game's: --out must be outside the repository.
"""
import argparse
import json
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import layer_inventory as inv  # noqa: E402
import map_tour  # noqa: E402

# name -> (what it switches, env, settings). The last few are diagnostics, not GS-exact mechanisms: the driver's colour
# compression and HiZ turned off (whether today's images get them at all), anisotropic filtering at 1x (what the
# world's texturing costs)
VARIANTS = {
    "exact": ("today's path: the depth rule (shader interlock), shader blends, exact Z's planes", {}, {}),
    "exactz": ("exact Z with decals settled instead of the depth rule (no interlock)", {"KF4_PC_DEPTH": "exact"}, {}),
    "nosettle": ("exact Z, decals not settled", {"KF4_PC_DEPTH": "exact", "KF4_PC_DECAL_SETTLE": "0"}, {}),
    "noplanes": ("the depth rule, the native draws' Z interpolated (no planes pass)", {"KF4_PC_PLANES": "0"}, {}),
    "gpu": ("the GPU's depth test: no rule, no exact Z, no planes", {"KF4_PC_DEPTH": "gpu"}, {}),
    "noblend": ("the depth rule, blends with no Vulkan form as the nearest Vulkan blend", {"KF4_PC_SHADER_BLEND": "0"},
                {}),
    "plain": ("the GPU's depth test and the nearest Vulkan blends", {"KF4_PC_DEPTH": "gpu", "KF4_PC_SHADER_BLEND": "0"},
              {}),
    "exact_nodcc": ("today's path, RADV's colour compression off", {"RADV_DEBUG": "nodcc"}, {}),
    "gpu_nodcc": ("the GPU's depth test, RADV's colour compression off", {"KF4_PC_DEPTH": "gpu", "RADV_DEBUG": "nodcc"},
                  {}),
    "gpu_nohiz": ("the GPU's depth test, RADV's HiZ off", {"KF4_PC_DEPTH": "gpu", "RADV_DEBUG": "nohiz"}, {}),
    "gpu_aniso1": ("the GPU's depth test, anisotropic filtering 1x", {"KF4_PC_DEPTH": "gpu"}, {"native_aniso": 1}),
    # The prototype plain path (pcr_canvas.cpp's KF4_PC_PLAIN_WORLD): the world into images of its own, optimal
    # layouts, attachment use alone, cleared by the pass; the picture lacks the world
    "opaque": ("the GPU's depth test, the world's batches unblended, in the frame's surfaces",
               {"KF4_PC_DEPTH": "gpu", "KF4_PC_SHADER_BLEND": "0", "KF4_PC_PLAIN_WORLD": "opaque"}, {}),
    "plainworld": ("the world into images of its own (optimal, compressible), the GPU's depth test",
                   {"KF4_PC_DEPTH": "gpu", "KF4_PC_SHADER_BLEND": "0", "KF4_PC_PLAIN_WORLD": "1"}, {}),
    "plainworld_nodcc": ("the world into images of its own, RADV's colour compression off",
                         {"KF4_PC_DEPTH": "gpu", "KF4_PC_SHADER_BLEND": "0", "KF4_PC_PLAIN_WORLD": "1",
                          "RADV_DEBUG": "nodcc"}, {}),
    "plainopaque": ("... and drawn unblended", {"KF4_PC_DEPTH": "gpu", "KF4_PC_SHADER_BLEND": "0",
                                                "KF4_PC_PLAIN_WORLD": "unblended"}, {}),
}
MAIN = "exact,exactz,nosettle,noplanes,gpu,noblend,plain"

NATIVE = re.compile(r"\[stats\] pc native draws: (\d+) draws, (\d+) batches \((\d+) triangles\)")


def world_split(r):
    """GPU ms a game frame by part: the world, its DATE draws, the sky, the scrolling targets (native 280), the
    2D draws (layers, HUD), copies, the picture and present, and all"""
    frames = r.get("game_frames") or 0
    if not frames or not r.get("shaders"):
        return None
    ms = lambda ns: ns / 1e6 / frames  # noqa: E731
    parts = dict(world=0.0, planes=0.0, date=0.0, sky=0.0, n280=0.0, d2=0.0, copies=0.0, scanout=0.0, other=0.0)
    for state, (ns, _) in r["shaders"].items():
        if state.startswith("native 000"):
            parts["date" if "DATE" in state else "sky" if "surface copy" in state else "world"] += ms(ns)
    for kind, (ns, _) in r["kinds"].items():
        k = kind[3:] if kind.startswith("pc ") else kind
        if k == "native 000":
            continue
        if k == "planes":
            parts["planes"] += ms(ns)
        elif k in ("native 280", "native 700"):
            parts["n280"] += ms(ns)
        elif k == "2d":
            parts["d2"] += ms(ns)
        elif k in ("texture copy", "surface", "upload"):
            parts["copies"] += ms(ns)
        elif k in ("picture", "present"):
            parts["scanout"] += ms(ns)
        else:
            parts["other"] += ms(ns)
    parts["world_all"] = parts["world"] + parts["n280"] + parts["planes"]
    parts["total"] = sum(ns for ns, _ in r["kinds"].values()) / 1e6 / frames
    parts["fps"] = frames / r["seconds"]
    return parts


def native_counts(folder, stem):
    path = os.path.join(folder, stem + "_stderr.txt")
    if os.path.exists(path):
        for line in open(path, errors="replace"):
            m = NATIVE.search(line)
            if m:
                return [int(x) for x in m.groups()]
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True)
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--exe")
    p.add_argument("--scenes", default="start,pond,palace")
    p.add_argument("--variants", default=MAIN)
    p.add_argument("--scales", default="1,2,4")
    p.add_argument("--rates", default="30")
    p.add_argument("--rounds", type=int, default=1)
    p.add_argument("--seconds", type=int, default=4)
    p.add_argument("--report-only", action="store_true")
    a = p.parse_args()
    if a.variants == "list":
        for n, (what, env, settings) in VARIANTS.items():
            print("%-12s %s; %s" % (n, what, " ".join(["%s=%s" % kv for kv in env.items()] +
                                                      ["%s: %s" % kv for kv in settings.items()])))
        return
    if not a.disc and not a.report_only:
        sys.exit("world_plain: pass --disc or set KF4_DISC")
    a.out = os.path.abspath(a.out)
    if a.out == ROOT or a.out.startswith(ROOT + os.sep):
        sys.exit("world_plain: --out must be outside the repository")
    os.makedirs(a.out, exist_ok=True)
    scenes = [s.strip() for s in a.scenes.split(",") if s.strip()]
    variants = [v.strip() for v in a.variants.split(",") if v.strip()]
    scales = [int(s) for s in a.scales.split(",") if s.strip()]
    rates = [int(r) for r in a.rates.split(",") if r.strip()]
    # what layer_inventory's live step reads from its arguments
    a.live_seconds, a.live_newgame, a.force = a.seconds, False, False
    if not a.report_only:
        a.exe = a.exe or map_tour.find_exe("kf4")
        base_path = os.path.join(a.out, "base.kf4quick")
        if not os.path.exists(base_path):
            map_tour.make_base(a.exe, a.disc, a.out, None)
        a.base = map_tour.read_base(base_path)
        a.base_path = base_path
        for rnd in range(a.rounds):
            for scene in scenes:
                for scale in scales:
                    for v in variants:
                        for rate in rates:
                            tag = "%s_s%d_r%d" % (v, scale, rnd)
                            r = inv.live_scenario(a, scene, rate, tag, VARIANTS[v][1],
                                                  dict(VARIANTS[v][2], native_scale=scale))
                            w = world_split(r) if r else None
                            print("%-8s %dx %-9s %d round %d: %s" % (
                                scene, scale, v, rate, rnd, "world %.2f, all %.2f ms a game frame" % (
                                    w["world_all"], w["total"]) if w else "no profile"), flush=True)
    report(a, scenes, variants, scales, rates)


def report(a, scenes, variants, scales, rates):
    rows = []
    for scene in scenes:
        folder = os.path.join(a.out, "scenarios", inv.slug(scene))
        for scale in scales:
            for rate in rates:
                for v in variants:
                    runs = []
                    for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
                        m = re.fullmatch(r"live_%s_s%d_r(\d+)_%d\.json" % (re.escape(v), scale, rate), f)
                        if m:
                            r = json.load(open(os.path.join(folder, f)))
                            w = world_split(r)
                            if w:
                                w["native"] = native_counts(folder, f[:-5])
                                runs.append(w)
                    if not runs:
                        continue
                    med = {k: statistics.median(x[k] for x in runs) for k in runs[0] if k != "native"}
                    med["spread"] = max(x["world_all"] for x in runs) - min(x["world_all"] for x in runs)
                    med["n"] = len(runs)
                    nat = runs[0]["native"]
                    rows.append(dict(scene=scene, scale=scale, rate=rate, variant=v, native=nat, **med))
    json.dump(rows, open(os.path.join(a.out, "world_plain.json"), "w"), indent=1)
    head = ("| Scene | Scale | Rate | Variant | 0x000 | 0x280, 0x700 | Planes | World | DATE | Sky | 2D | Copies | "
            "Scanout | All | Runs (spread) |")
    lines = [head, "|" + "---|" * (head.count("|") - 1)]
    for r in rows:
        lines.append("| %s | %dx | %d | %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %d (%.2f) |"
                     % (r["scene"], r["scale"], r["rate"], r["variant"], r["world"], r["n280"], r["planes"],
                        r["world_all"], r["date"], r["sky"], r["d2"], r["copies"], r["scanout"], r["total"], r["n"],
                        r["spread"]))
    text = "\n".join(lines) + "\n"
    open(os.path.join(a.out, "world_plain.md"), "w").write(text)
    print(text)


if __name__ == "__main__":
    main()
