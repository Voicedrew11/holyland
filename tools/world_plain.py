#!/usr/bin/env python3
"""What the world costs on the GPU drawn the GS's way and with the GS-exact mechanisms switched off
(docs/plans/world-plain.md).

    tools/world_plain.py --out DIR [--disc ISO] [--scenes start,pond,palace] [--variants exact,gpu,...|list]
                         [--scales 1,2,4] [--rates 30] [--rounds N] [--report-only] [--exe-for VARIANT=EXE,...]

Each scene of tools/layer_inventory.py played in a real window with the owner's settings (its live step: a copy of
the data folder, KF4_READ_SETTINGS=1, KF4_GPU_DETAIL=1), once for each variant (developers' KF4_PC_* switches, off the
exact path: for timing only) at each native scale. A variant may carry extra command-line arguments for the game as an
optional fourth element (e.g. ["--renderer", "scene"]), appended after the scenario's own. The rounds run the whole
matrix again, in the same order, so a drift of the GPU's clocks shows as a spread rather than as a variant's cost.

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
    # The depth by layers' proof of concept (docs/plans/depth-by-layers.md): the GPU's depth test, the layers (decals)
    # drawn a little nearer (pcr_canvas.cpp's KF4_PC_DEPTH=layers)
    "layers": ("the GPU's depth test, Z clamped at each pixel, the layers drawn 1 Z nearer (depth by layers)",
               {"KF4_PC_DEPTH": "layers"}, {}),
    "unorm": ("the GPU's depth test in 16-bit steps (D16_UNORM: the GS's whole Z), Z clamped at each pixel, DATE by a "
              "copy", {"KF4_PC_DEPTH": "unorm"}, {}),
    # The GS's arithmetic in the shader, as a ceiling (docs/plans/scene-renderer-design.md): exact Z's whole Z alone at
    # each pixel (no fine part, no decal settled, no rule), the depth written by the shader, DATE by the stencil
    "gsexact": ("exact Z's whole Z alone at each pixel, written by the shader (the GS's arithmetic: a ceiling)",
                {"KF4_PC_DEPTH": "gsexact"}, {}),
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
    # The scene renderer (docs/plans/scene-renderer-design.md): the game's arguments, no KF4_PC_* switch (it ignores them)
    "scene": ("the scene renderer (--renderer scene, docs/plans/scene-renderer-design.md)", {}, {},
              ["--renderer", "scene"]),
    # ... with one of slice 2's folds off (scene-slice2-results.md): the alpha's reset drawn, the brightness drawn into
    # the target, each clear a step where it is met
    "scene_noalpha": ("the scene renderer, the alpha's reset drawn (KF4_SCN_FOLD_ALPHA=0)", {"KF4_SCN_FOLD_ALPHA": "0"},
                      {}, ["--renderer", "scene"]),
    "scene_nofold": ("the scene renderer, the brightness drawn into the target (KF4_SCN_FOLD=0)", {"KF4_SCN_FOLD": "0"},
                     {}, ["--renderer", "scene"]),
    "scene_nohold": ("the scene renderer, each clear a step where met (KF4_SCN_HOLD_CLEARS=0)",
                     {"KF4_SCN_HOLD_CLEARS": "0"}, {}, ["--renderer", "scene"]),
    # ... the first-draw image (DATE's shadows) cleared every frame (scene-slice2-after.md)
    "scene_firstclear": ("the scene renderer, the first-draw image cleared every frame (KF4_SCN_FIRST_KEEP=0)",
                         {"KF4_SCN_FIRST_KEEP": "0"}, {}, ["--renderer", "scene"]),
    "scene_nofills": ("the scene renderer, the fills after the HUD drawn into the target (KF4_SCN_FOLD_FILLS=0)",
                      {"KF4_SCN_FOLD_FILLS": "0"}, {}, ["--renderer", "scene"]),
    # The same of another build, given by --exe-for scene_ref=EXE (an earlier slice's, measured in the same rounds)
    "scene_ref": ("the scene renderer of the build --exe-for names (an earlier slice's)", {}, {},
                  ["--renderer", "scene"]),
    # ... each draw's fields as push constants and its textures in a set 1 of its own, as slice 2 (slice 3's records
    # and bindless table off: HANDOFF_scene_slice3.md, 3a)
    "scene_norecords": ("the scene renderer, a draw's fields as push constants (KF4_SCN_RECORDS=0)",
                        {"KF4_SCN_RECORDS": "0"}, {}, ["--renderer", "scene"]),
    # The GPU's time by kind of work alone (KF4_GPU_DETAIL=0): the detail's timestamps at each change of shader state
    # wait for the GPU, and program 0x280 changes state with every pass (scene-slice2-after.md, "Slice 3's start"); the
    # world is then the native draws' kinds whole (0x000's DATE and sky in it), "All" the figure to read
    "scene_nodetail": ("the scene renderer, the GPU's time by kind alone (KF4_GPU_DETAIL=0)", {"KF4_GPU_DETAIL": "0"},
                       {}, ["--renderer", "scene"]),
    "scene_ref_nodetail": ("the scene renderer of the build --exe-for names, the GPU's time by kind alone",
                           {"KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
    # ... every call its own vkCmdDraw, not the runs as indirect draws (slice 3b: KF4_SCN_INDIRECT=0)
    "scene_noindirect": ("the scene renderer, a draw call a call, no indirect draws (KF4_SCN_INDIRECT=0)",
                         {"KF4_SCN_INDIRECT": "0"}, {}, ["--renderer", "scene"]),
    "scene_noblendruns": ("the scene renderer, no unblended call in a run blended by the source's alpha "
                          "(KF4_SCN_BLEND_RUNS=0)", {"KF4_SCN_BLEND_RUNS": "0"}, {}, ["--renderer", "scene"]),
    "scene_noblendruns_nodetail": ("the scene renderer, no unblended call in a blended run, the GPU's time by kind alone",
                                   {"KF4_SCN_BLEND_RUNS": "0", "KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
    "scene_noindirect_nodetail": ("the scene renderer, no indirect draws, the GPU's time by kind alone",
                                  {"KF4_SCN_INDIRECT": "0", "KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
    # ... 0x280's sphere passes not drawn (slice 3d's bound: the most fusing its two passes could save; the picture
    # lacks the shine)
    "scene_nosphere": ("the scene renderer, 0x280's sphere passes not drawn (KF4_SCN_NO_SPHERE=1: a bound)",
                       {"KF4_SCN_NO_SPHERE": "1"}, {}, ["--renderer", "scene"]),
    "scene_nosphere_nodetail": ("the scene renderer, 0x280's sphere passes not drawn, the GPU's time by kind alone",
                                {"KF4_SCN_NO_SPHERE": "1", "KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
    # ... 0x280's two passes as one draw where they can be (slice 3d, KF4_SCN_FUSE_280=1)
    "scene_fuse": ("the scene renderer, 0x280's two passes as one draw (KF4_SCN_FUSE_280=1)", {"KF4_SCN_FUSE_280": "1"},
                   {}, ["--renderer", "scene"]),
    # Slice 4's bound (scene-slice4-results.md): each picture between records the frame's whole scene again, unmoved, into
    # the same targets (KF4_SCN_BETWEEN_BOUND=1): what a picture between from the scene costs at most; wrong pictures.
    # Run at 30 and 60: the difference a game frame is a picture between's GPU time
    "scene_bound": ("the scene renderer, each picture between the frame's scene again (KF4_SCN_BETWEEN_BOUND=1: a bound)",
                    {"KF4_SCN_BETWEEN_BOUND": "1"}, {}, ["--renderer", "scene"]),
    "scene_bound_nodetail": ("the scene renderer, each picture between the frame's scene again, the GPU's time by kind "
                             "alone", {"KF4_SCN_BETWEEN_BOUND": "1", "KF4_GPU_DETAIL": "0"}, {},
                             ["--renderer", "scene"]),
    # Slice 3f's cheap tests (scene-slice4-results.md): what the world's 7 ms a game frame is made of. Each the GPU's time
    # by kind alone; the pictures of the first two are wrong
    "scene_flat_nodetail": ("the scene renderer, its draws shaded one colour (KF4_SCN_FLAT=1: the shading's bound)",
                            {"KF4_SCN_FLAT": "1", "KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
    "scene_tiny_nodetail": ("the scene renderer, its draws' scissor a pixel (KF4_SCN_TINY=1: no pixels, the vertex "
                            "stage and the draws left)", {"KF4_SCN_TINY": "1", "KF4_GPU_DETAIL": "0"}, {},
                            ["--renderer", "scene"]),
    "scene_aniso1_nodetail": ("the scene renderer, anisotropic filtering 1x", {"KF4_GPU_DETAIL": "0"},
                              {"native_aniso": 1}, ["--renderer", "scene"]),
    "scene_nodcc_nodetail": ("the scene renderer, RADV's colour compression off", {"RADV_DEBUG": "nodcc",
                                                                                  "KF4_GPU_DETAIL": "0"}, {},
                             ["--renderer", "scene"]),
    "scene_nohiz_nodetail": ("the scene renderer, RADV's HiZ off", {"RADV_DEBUG": "nohiz", "KF4_GPU_DETAIL": "0"}, {},
                             ["--renderer", "scene"]),
    "scene_fuse_nodetail": ("the scene renderer, 0x280's two passes as one draw, the GPU's time by kind alone",
                            {"KF4_SCN_FUSE_280": "1", "KF4_GPU_DETAIL": "0"}, {}, ["--renderer", "scene"]),
}
MAIN = "exact,exactz,nosettle,noplanes,gpu,noblend,plain"

NATIVE = re.compile(r"\[stats\] pc native draws: (\d+) draws, (\d+) batches \((\d+) triangles\)")


def world_split(r):
    """GPU ms a game frame by part: the world, its DATE draws, the sky, the scrolling targets (native 280), the
    2D draws (layers, HUD), copies, the picture and present, and all"""
    frames = r.get("game_frames") or 0
    if not frames or not r.get("kinds"):
        return None
    ms = lambda ns: ns / 1e6 / frames  # noqa: E731
    parts = dict(world=0.0, planes=0.0, date=0.0, sky=0.0, n280=0.0, n280_only=0.0, d2=0.0, copies=0.0, scanout=0.0,
                 other=0.0)
    for state, (ns, _) in r.get("shaders", {}).items():
        if state.startswith("native 000"):
            parts["date" if "DATE" in state else "sky" if "surface copy" in state else "world"] += ms(ns)
    detail = bool(r.get("shaders"))
    for kind, (ns, _) in r["kinds"].items():
        k = kind[3:] if kind.startswith("pc ") else kind
        if k == "native 000":
            if not detail:
                parts["world"] += ms(ns)  # (no shader states: the kind whole, its DATE and sky in it)
            continue
        if k == "planes":
            parts["planes"] += ms(ns)
        elif k in ("native 280", "native 700"):
            parts["n280"] += ms(ns)
            if k == "native 280":
                parts["n280_only"] += ms(ns)
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
    p.add_argument("--exe-for", default="",
                   help="VARIANT=EXE,...: a variant played by another build (e.g. scene_ref=out/slice1/kf4)")
    a = p.parse_args()
    if a.variants == "list":
        for n, v in VARIANTS.items():
            what, env, settings = v[:3]
            args = v[3] if len(v) > 3 else []
            print("%-12s %s; %s" % (n, what, " ".join(["%s=%s" % kv for kv in env.items()] +
                                                      ["%s: %s" % kv for kv in settings.items()] +
                                                      (["args: %s" % " ".join(args)] if args else []))))
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
        exe_for = dict(kv.split("=", 1) for kv in a.exe_for.split(",") if kv)
        own_exe = a.exe
        for rnd in range(a.rounds):
            for scene in scenes:
                for scale in scales:
                    for v in variants:
                        for rate in rates:
                            tag = "%s_s%d_r%d" % (v, scale, rnd)
                            a.exe = exe_for.get(v, own_exe)
                            r = inv.live_scenario(a, scene, rate, tag, VARIANTS[v][1],
                                                  dict(VARIANTS[v][2], native_scale=scale),
                                                  args_more=VARIANTS[v][3] if len(VARIANTS[v]) > 3 else [])
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
                    med["spread_all"] = max(x["total"] for x in runs) - min(x["total"] for x in runs)
                    med["n"] = len(runs)
                    nat = runs[0]["native"]
                    rows.append(dict(scene=scene, scale=scale, rate=rate, variant=v, native=nat, **med))
    json.dump(rows, open(os.path.join(a.out, "world_plain.json"), "w"), indent=1)
    head = ("| Scene | Scale | Rate | Variant | 0x000 | 0x280, 0x700 | 0x280 | Planes | World | DATE | Sky | 2D | Copies | "
            "Scanout | All | Runs (spread) | All's spread |")
    lines = [head, "|" + "---|" * (head.count("|") - 1)]
    for r in rows:
        lines.append("| %s | %dx | %d | %s | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %d (%.2f) "
                     "| %.2f |"
                     % (r["scene"], r["scale"], r["rate"], r["variant"], r["world"], r["n280"], r.get("n280_only", 0.0),
                        r["planes"],
                        r["world_all"], r["date"], r["sky"], r["d2"], r["copies"], r["scanout"], r["total"], r["n"],
                        r["spread"], r.get("spread_all", 0.0)))
    text = "\n".join(lines) + "\n"
    open(os.path.join(a.out, "world_plain.md"), "w").write(text)
    print(text)


if __name__ == "__main__":
    main()
