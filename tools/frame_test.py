#!/usr/bin/env python3
"""Step 2's check: the game's pictures, drawn by paraLLEl-GS, the same every run.

    tools/frame_test.py [--disc ISO] [--exe PATH] [--keep DIR] [--frames N,N,...] [--dense A-B] [--runs N]
                        [--renderer gs|native|modern|fast]

Plays tests/newgame.pad as tools/boot_test.py does, headless with a fixed clock, and writes the picture
shown at a few vblanks (--dump-frames): the Agetec logo, the title menu, the brightness screen, the
opening movie, the opening, and the first map. Runs twice (--runs) and compares the pictures byte for byte, then checks none
of them is a single colour. It also writes every picture of a stretch of the opening (--dense, "none"
for none), where the frame's clear, its scene and its overlays come by both DMA channels, so the order
the GS gets them in is checked frame by frame. Then one more run dumps the last of the pictures alone,
which must be byte for byte the one the runs that dumped them all wrote: reading a picture back must
leave nothing behind in the GS (the native GS's read-backs write its render targets back to VRAM). Needs
a Vulkan GPU that paraLLEl-GS runs on. The pictures are kept with --keep, for looking at; the disc comes
from --disc or KF4_DISC.

The runs ask for --renderer gs: the program's default is the native GS since stage 4, and this test is
paraLLEl-GS's (the reference's) determinism. --renderer native runs the same checks on the native GS; modern and
fast on its scene and on the PC renderer (docs/port/PC_RENDERER.md).
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")
# the logo, the title menu, the brightness screen, the opening movie, the opening, the first map
FRAMES = "200,500,700,740,1000,2000"
# every vblank of a stretch of the opening
DENSE = "990-1010"


def find_exe():
    for preset in ("linux-gcc", "linux-clang", "windows-clang"):
        for name in ("kf4", "kf4.exe"):
            path = os.path.join(ROOT, "out", preset, name)
            if os.path.exists(path):
                return path
    sys.exit("frame_test: no built program under out/; build one or pass --exe")


STARTED = {"gs": ("[gs] paraLLEl-GS on", "paraLLEl-GS"), "native": ("[gsn] the native GS on", "the native GS"),
           "modern": ("[gsn] the native GS on", "the native GS's scene (modern)"),
           "fast": ("[gsn] the native GS on", "the PC renderer (fast)")}


def run(exe, disc, frames, folder, renderer):
    os.makedirs(folder, exist_ok=True)
    last = max(int(n) for n in frames.split(","))
    cmd = [exe, "--disc", disc, "--fast", "--headless", "--frames", str(last + 1),
           "--clock", "2026-01-01T12:00:00", "--memcard", "none", "--no-audio", "--input", PAD,
           "--dump-frames", frames, "--dump-dir", folder, "--renderer", renderer]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.exit("frame_test: the program exited with %d" % result.returncode)
    started, name = STARTED[renderer]
    if started not in result.stdout:
        sys.stdout.write(result.stdout)
        sys.exit("frame_test: %s did not start (no Vulkan GPU, or built without it)" % name)


def colours(path):
    """How many colours the picture has (1: a blank screen)."""
    with open(path, "rb") as f:
        data = f.read()
    width, height = struct.unpack(">II", data[16:24])
    at = data.index(b"IDAT")
    size = struct.unpack(">I", data[at - 4:at])[0]
    raw = zlib.decompress(data[at + 4:at + 4 + size])
    row = width * 3 + 1
    seen = set()
    for y in range(0, height, 4):
        line = raw[y * row + 1:(y + 1) * row]
        for x in range(0, width * 3, 12):
            seen.add(line[x:x + 3])
    return len(seen)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--frames", default=FRAMES, help="the vblanks to dump (default %s)" % FRAMES)
    parser.add_argument("--dense", default=DENSE, help="also every vblank from A to B (default %s)" % DENSE)
    parser.add_argument("--runs", type=int, default=2, help="runs to compare (default 2)")
    parser.add_argument("--keep", help="write the pictures into this folder")
    parser.add_argument("--renderer", default="gs", choices=sorted(STARTED),
                        help="who draws (default gs, paraLLEl-GS, whatever the program's default)")
    args = parser.parse_args()
    # never the player's data folder: settings, saves and caches go to a temporary one
    os.environ.setdefault("KF4_DATA_DIR", tempfile.mkdtemp(prefix="kf4_data_"))
    if not args.disc:
        sys.exit("frame_test: pass --disc or set KF4_DISC")
    if args.runs < 2:
        sys.exit("frame_test: --runs wants 2 or more")
    exe = args.exe or find_exe()

    frames = [int(n) for n in args.frames.split(",")]
    dense = []
    if args.dense != "none":
        first, last = (int(n) for n in args.dense.split("-"))
        dense = [n for n in range(first, last + 1) if n not in frames]
    folder = args.keep or tempfile.mkdtemp(prefix="kf4_frames_")
    run_names = ["run%d" % i for i in range(args.runs)]
    for run_name in run_names:
        run(exe, args.disc, ",".join(str(n) for n in sorted(frames + dense)), os.path.join(folder, run_name),
            args.renderer)
    # the last picture alone: the read-backs of the others must not have changed it
    last = max(frames)
    run(exe, args.disc, str(last), os.path.join(folder, "alone"), args.renderer)

    ok = True
    dense_differ = []
    for n in sorted(frames + dense):
        name = "frame_%06d.png" % n
        paths = [os.path.join(folder, run_name, name) for run_name in run_names]
        if not all(os.path.exists(p) for p in paths):
            ok = False
            print("vblank %5d: no picture" % n)
            continue
        digests = [hashlib.sha256(open(p, "rb").read()).hexdigest() for p in paths]
        same = len(set(digests)) == 1
        if n not in frames:
            if not same:
                ok = False
                dense_differ.append(n)
            continue
        count = colours(paths[0])
        notes = []
        if not same:
            ok = False
            notes.append("NOT THE SAME every run")
        if count < 2:
            ok = False
            notes.append("one colour")
        print("vblank %5d: %s %5d colours %s" % (n, digests[0][:16], count, " ".join(notes)))
    if dense:
        if dense_differ:
            print("vblanks %s: NOT THE SAME every run: %s" % (args.dense, " ".join(str(n) for n in dense_differ)))
        else:
            print("vblanks %s: the same every run" % args.dense)
    name = "frame_%06d.png" % last
    alone, together = os.path.join(folder, "alone", name), os.path.join(folder, run_names[0], name)
    if not os.path.exists(alone) or not os.path.exists(together) or \
            open(alone, "rb").read() != open(together, "rb").read():
        ok = False
        print("vblank %5d alone: NOT THE SAME as with the other pictures dumped" % last)
    else:
        print("vblank %5d alone: the same as with the other pictures dumped" % last)
    print("pictures in %s" % folder)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
