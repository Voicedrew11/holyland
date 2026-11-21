#!/usr/bin/env python3
"""Two ways of the final pass against each other (docs/plans/scene-final-pass-results.md): the pictures as a window
shows them (--dump-present), headless, each case of tools/motion_compare.py played by both, and every picture compared.
By default the final pass in one (pcr.cpp's PresentScaler::fused, KF4_PC_FUSED=1) against the passes one by one
(=0); --ref-exe plays the second by another build (an earlier one: what a change to the passes changed).

    tools/final_pass_compare.py --out DIR [--exe EXE] [--ref-exe EXE] [--env-a K=V,...] [--env-b K=V,...]
                                [--disc ISO] [--cases a,b] [--sizes 2:1920x1080,...]
                                [--renderer scene|pc] [--rate 60] [--antialias fxaa] [--sharpen 0.5]

A size is SCALE:WxH: the native scale (2 and 4 with native widescreen, 1 without) and the window's picture. The
defaults take each of fused's ways: down alone (2x widescreen, 1920 x 896, into 1920 x 1080), both directions smaller
(4x widescreen into 1920 x 1080), both larger (1x into 1920 x 1080).

Into DIR: report.md, and for each case and size the pictures that differ most, side by side (fused | passes |
difference x 16: a | b | difference). Needs Pillow and numpy.
"""
import argparse
import os
import shutil
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import motion_compare as mc  # noqa: E402

DEFAULT_CASES = "menus,combat,first kill,spell 3,fire elementals,underwater"
DEFAULT_SIZES = "2:1920x1080,4:1920x1080,1:1920x1080"


def options(scale, size, a):
    o = ["--native-scale", str(scale), "--native-draw", "on", "--antialias", a.antialias,
         "--present-sharpen", str(a.sharpen), "--present-scaling", "best", "--dump-present", size]
    if scale > 1:
        o += ["--widescreen", "--native-widescreen"]
    return o


def pictures(d):
    return {f: os.path.join(d, f) for f in os.listdir(d) if f.endswith((".png", ".ppm", ".bmp"))}


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True)
    p.add_argument("--exe")
    p.add_argument("--ref-exe", help="the second way's build (default: --exe)")
    p.add_argument("--env-a", default="KF4_PC_FUSED=1", help="the first way's environment, K=V,...")
    p.add_argument("--env-b", default="KF4_PC_FUSED=0", help="the second way's")
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--cases", default=DEFAULT_CASES)
    p.add_argument("--sizes", default=DEFAULT_SIZES)
    p.add_argument("--renderer", default="scene")
    p.add_argument("--rate", type=int, default=60)
    p.add_argument("--antialias", default="fxaa")
    p.add_argument("--sharpen", type=float, default=0.5)
    p.add_argument("--top", type=int, default=3)
    a = p.parse_args()
    a.out = os.path.abspath(a.out)
    if a.out == ROOT or a.out.startswith(ROOT + os.sep):
        sys.exit("final_pass_compare: --out must be outside the repository")
    a.exe = a.exe or os.path.join(ROOT, "out", "linux-clang", "kf4")
    own_exe = a.exe
    # (what motion_compare.run reads)
    a.renderer_name, a.pc_extra, a.ties = a.renderer, "", False
    os.makedirs(os.path.join(a.out, "evidence"), exist_ok=True)
    lines = ["| Case | Size | Pictures | The same | Differing | Pixels differing | Most a channel differs |",
             "|---|---|---|---|---|---|---|"]
    for name in [c.strip() for c in a.cases.split(",") if c.strip()]:
        case = mc.CASES[name]
        frames = sorted(set(case["frames"]))
        for spec in a.sizes.split(","):
            scale, size = spec.split(":")
            scale = int(scale)
            runs = {}
            for way, exe, env in (("1", own_exe, a.env_a), ("0", a.ref_exe or own_exe, a.env_b)):
                d = os.path.join(a.out, "runs", "%s_%dx_%s_%s" % (mc.slug(name), scale, size, "ab"[way == "0"]))
                saved = dict(os.environ)
                os.environ.update(dict(kv.split("=", 1) for kv in env.split(",") if kv))
                a.renderer, a.exe = a.renderer_name, exe
                rc = mc.run(a, case, d, "pc", options(scale, size, a), frames, rate=a.rate)
                os.environ.clear()
                os.environ.update(saved)
                runs[way] = d
                if rc != 0:
                    print("%s %s way %s: exit %d" % (name, spec, "ab"[way == "0"], rc), flush=True)
            one, passes = pictures(runs["1"]), pictures(runs["0"])
            same = differing = 0
            pixels = 0
            most = 0
            worst = []
            for f in sorted(set(one) & set(passes)):
                x = np.asarray(Image.open(one[f]).convert("RGB"), dtype=np.int16)
                y = np.asarray(Image.open(passes[f]).convert("RGB"), dtype=np.int16)
                if x.shape != y.shape:
                    differing += 1
                    worst.append((1 << 30, f))
                    continue
                diff = np.abs(x - y)
                n = int(np.count_nonzero(diff.max(axis=2)))
                if n == 0:
                    same += 1
                    continue
                differing += 1
                pixels += n
                most = max(most, int(diff.max()))
                worst.append((n, f))
            for n, f in sorted(worst, reverse=True)[:a.top]:
                x, y = Image.open(one[f]).convert("RGB"), Image.open(passes[f]).convert("RGB")
                if x.size == y.size:
                    d = np.clip(np.abs(np.asarray(x, dtype=np.int16) - np.asarray(y, dtype=np.int16)) * 16, 0, 255)
                    sheet = Image.new("RGB", (x.width * 3, x.height))
                    sheet.paste(x, (0, 0))
                    sheet.paste(y, (x.width, 0))
                    sheet.paste(Image.fromarray(d.astype(np.uint8)), (2 * x.width, 0))
                    sheet.save(os.path.join(a.out, "evidence", "%s_%dx_%s_%s.png" % (mc.slug(name), scale, size,
                                                                                      os.path.splitext(f)[0])))
            total = len(set(one) & set(passes))
            line = "| %s | %dx %s | %d | %d | %d | %d | %d |" % (name, scale, size, total, same, differing, pixels,
                                                                 most)
            print(line, flush=True)
            lines.append(line)
            for d in runs.values():
                for f in pictures(d).values():
                    os.remove(f)
    text = "\n".join(lines) + "\n"
    open(os.path.join(a.out, "report.md"), "w").write(text)
    print(text)


if __name__ == "__main__":
    main()
