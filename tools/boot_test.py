#!/usr/bin/env python3
"""Step 1's check: boot headless to a new game, and check the run is deterministic.

    tools/boot_test.py [--disc ISO] [--exe PATH] [--frames N] [--keep DIR]

Runs the program twice with tests/newgame.pad as the pad, at full speed with nothing drawn and a fixed
clock, and compares the two logs (each line is a vblank's events or the game's state that frame). Then
checks the log goes title -> New Game -> the first map: the title's files load, the opening movie ends,
099.map loads, and the player stands in map 0x63 with 50 HP. The disc comes from --disc or KF4_DISC; the
program from --exe, or out/linux-gcc, out/linux-clang or out/windows-clang, whichever exists.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")

# what the log has to show, in order
STAGES = [
    ("the title", re.compile(r"event: read KF4\.DAT:title/title_menu1\.tm2")),
    ("New Game: the opening movie", re.compile(r"event: movie: ")),
    ("the first map's file", re.compile(r"event: read KF4\.DAT:099\.map")),
    ("the player in map 0x63", re.compile(r"frame=[1-9]\d* .*map=0:63 pos=\S+ .*hp=50 ")),
]


def find_exe():
    for preset in ("linux-gcc", "linux-clang", "windows-clang"):
        for name in ("kf4", "kf4.exe"):
            path = os.path.join(ROOT, "out", preset, name)
            if os.path.exists(path):
                return path
    sys.exit("boot_test: no built program under out/; build one or pass --exe")


def run(exe, disc, frames, log):
    cmd = [exe, "--disc", disc, "--fast", "--headless", "--frames", str(frames),
           "--clock", "2026-01-01T12:00:00", "--input", PAD, "--log", log]
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.exit("boot_test: the program exited with %d" % result.returncode)
    with open(log) as f:
        return f.read().splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--frames", type=int, default=3000, help="vblanks to run (default 3000)")
    parser.add_argument("--keep", help="write the two logs into this folder")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("boot_test: pass --disc or set KF4_DISC")
    exe = args.exe or find_exe()

    folder = args.keep or tempfile.mkdtemp(prefix="kf4_boot_")
    os.makedirs(folder, exist_ok=True)
    a = run(exe, args.disc, args.frames, os.path.join(folder, "boot_a.log"))
    b = run(exe, args.disc, args.frames, os.path.join(folder, "boot_b.log"))

    ok = True
    if a != b:
        ok = False
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                print("not deterministic: line %d differs\n  %s\n  %s" % (i + 1, x, y))
                break
        else:
            print("not deterministic: %d lines against %d" % (len(a), len(b)))
    else:
        print("deterministic: %d log lines, the same both runs" % len(a))

    line = 0
    for name, pattern in STAGES:
        while line < len(a) and not pattern.search(a[line]):
            line += 1
        if line == len(a):
            ok = False
            print("missing: %s" % name)
            break
        print("%-28s %s" % (name + ":", a[line]))
    if not args.keep:
        print("logs in %s" % folder)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
