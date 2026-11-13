#!/usr/bin/env python3
"""A GS capture of many frames (F9 with KF4_GS_CAPTURE_VBLANKS, or --gs-capture-at VBLANK:N) drawn by the PC renderer
and by paraLLEl-GS with kf4-gsreplay, and compared frame by frame (tools/README.md, "kf4-gsreplay").

    python3 tools/replay_compare.py CAPTURE.kf4gs --out DIR [--pc "SETTINGS"] [--decal-losses] [--worst N]
        [--gsreplay out/linux-clang/kf4-gsreplay]

--pc: kf4-gsreplay's options for the PC renderer's settings (the player's: --scale, --msaa, ...).
Each frame's PC picture is averaged down to the GS's size (a box per GS pixel) and compared with paraLLEl-GS's: the
mean difference, and the GS pixels that differ by more than 32 and 64 (of 255). A frame far from its neighbours is
what shows in some frames only. --decal-losses draws the PC frames again with kf4-gsreplay --decal-losses, and counts
each frame's surface pixels where a decal lost a near tie. DIR gets report.txt, and for the N frames that differ most
(and the N with the most decal losses) PC | paraLLEl-GS | difference side by side, worst_NNNNNN.png and
losses_NNNNNN.png (losses in magenta). The pictures replayed are kept in DIR/gs, DIR/pc (and DIR/pc_losses).
"""
import argparse
import os
import shlex
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def replay(gsreplay, capture, out, options):
    os.makedirs(out, exist_ok=True)
    cmd = [gsreplay, capture, "--frames", "--out", out] + options
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit("%s failed:\n%s" % (" ".join(cmd), result.stderr[-2000:]))


def frames(folder):
    return sorted(f for f in os.listdir(folder) if f.startswith("frame_") and f.endswith(".png"))


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def down(pc, shape):
    """the PC picture averaged to shape (a box of the scale per GS pixel)"""
    h, w = shape[:2]
    sy, sx = pc.shape[0] // h, pc.shape[1] // w
    return pc[: h * sy, : w * sx].astype(np.float64).reshape(h, sy, w, sx, 3).mean(axis=(1, 3))


def losses(with_losses, without):
    """surface pixels where a decal lost a near tie: red and blue raised by 16 a decal, green as it was"""
    d = with_losses - without
    return (d[:, :, 0] >= 8) & (d[:, :, 2] >= 8) & (np.abs(d[:, :, 1]) <= 1)


def side_by_side(pc, gs, path, mark=None):
    h, w = gs.shape[:2]
    small = down(pc, gs.shape)
    if mark is not None:
        sy, sx = pc.shape[0] // h, pc.shape[1] // w
        m = mark[: h * sy, : w * sx].reshape(h, sy, w, sx).any(axis=(1, 3))
        small[m] = (255, 0, 255)
    diff = np.clip(np.abs(down(pc, gs.shape) - gs) * 4, 0, 255)
    sheet = np.concatenate([small, gs, diff], axis=1).astype(np.uint8)
    Image.fromarray(sheet).save(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture")
    parser.add_argument("--out", required=True)
    parser.add_argument("--pc", default="")
    parser.add_argument("--decal-losses", action="store_true")
    parser.add_argument("--worst", type=int, default=6)
    parser.add_argument("--gsreplay", default=os.path.join(ROOT, "out", "linux-clang", "kf4-gsreplay"))
    args = parser.parse_args()
    pc_options = ["--renderer", "pc"] + shlex.split(args.pc)
    gs_dir, pc_dir, loss_dir = (os.path.join(args.out, d) for d in ("gs", "pc", "pc_losses"))
    replay(args.gsreplay, args.capture, gs_dir, [])
    replay(args.gsreplay, args.capture, pc_dir, pc_options)
    if args.decal_losses:
        replay(args.gsreplay, args.capture, loss_dir, pc_options + ["--decal-losses"])
    rows = []
    for name in frames(gs_dir):
        gs, pc = load(os.path.join(gs_dir, name)), load(os.path.join(pc_dir, name))
        d = np.abs(down(pc, gs.shape) - gs).mean(axis=2)
        lost = int(losses(load(os.path.join(loss_dir, name)), pc).sum()) if args.decal_losses else 0
        rows.append((name, float(d.mean()), int((d > 32).sum()), int((d > 64).sum()), lost))
    with open(os.path.join(args.out, "report.txt"), "w") as report:
        report.write("frame                 mean   >32   >64  decal-loss pixels\n")
        for name, mean, d32, d64, lost in rows:
            report.write("%-20s %5.2f %5d %5d %8d\n" % (name, mean, d32, d64, lost))
    print(open(os.path.join(args.out, "report.txt")).read(), end="")
    for name, *_ in sorted(rows, key=lambda r: -r[2])[: args.worst]:
        side_by_side(load(os.path.join(pc_dir, name)), load(os.path.join(gs_dir, name)),
                     os.path.join(args.out, "worst_" + name[6:]))
    if args.decal_losses:
        for name, *rest in sorted(rows, key=lambda r: -r[4])[: args.worst]:
            if rest[3] == 0:
                break
            pc = load(os.path.join(pc_dir, name))
            side_by_side(pc, load(os.path.join(gs_dir, name)), os.path.join(args.out, "losses_" + name[6:]),
                         losses(load(os.path.join(loss_dir, name)), pc))


if __name__ == "__main__":
    main()
