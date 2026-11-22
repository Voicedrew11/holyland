#!/usr/bin/env python3
"""Save states' check: a state saved in one run goes on in another exactly as the run that saved it.

    tools/state_test.py [--disc ISO] [--exe PATH] [--keep DIR] [--renderer gs|pc|scene]

Plays tests/newgame.pad with the automap on (its globals hold the host's memory: game/automap.c is linked
outside the state's image and writes its fog of war into the state itself), teleports to map 009 and saves
a state there (--state-save-at). Then the state is loaded twice: by a new run, before the game starts
(--state-load FILE), and by a run already playing another map, at a vblank (--state-load VBLANK:FILE, the
path F7 takes). Each must run on for a few hundred vblanks without crashing, its game state line at every
vblank (--log) the saving run's from the state's vblank on, and the pictures it writes (--dump-frames) the
saving run's byte for byte. The pictures are paraLLEl-GS's (--renderer gs, the default here): the PC
renderer's and the scene renderer's keep host-side surfaces a state does not hold, so after a load a few
pixels of theirs can differ by a level or two (pass --renderer to see). Headless, no sound, a fixed clock;
the disc comes from --disc or KF4_DISC.
"""
import argparse
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")
TELEPORT = "1900:9:0"  # map 009, its start
SAVE_AT = 2200
LOAD_AT = 2300         # the second load: the run is on the first map (0x63) by then
END = 2700
FRAMES = [SAVE_AT + 1, 2400, END]
LINE = re.compile(r"^(\d+) frame=")


def find_exe():
    for preset in ("linux-clang", "linux-gcc", "windows-clang"):
        for name in ("kf4", "kf4.exe"):
            path = os.path.join(ROOT, "out", preset, name)
            if os.path.exists(path):
                return path
    sys.exit("state_test: no built program under out/; build one or pass --exe")


def run(exe, disc, renderer, folder, extra):
    os.makedirs(folder, exist_ok=True)
    log = os.path.join(folder, "game.log")
    cmd = [exe, "--disc", disc, "--fast", "--headless", "--frames", str(END + 1), "--clock", "2026-01-01T12:00:00",
           "--saves", "none", "--no-audio", "--input", PAD, "--automap", "--invincible", "--renderer", renderer,
           "--log", log, "--dump-frames", ",".join(str(n) for n in FRAMES), "--dump-dir", folder] + extra
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with open(os.path.join(folder, "stderr.txt"), "w") as f:
        f.write(result.stdout)
    if result.returncode != 0:
        sys.stdout.write(result.stdout[-4000:])
        sys.exit("state_test: %s exited with %d" % (os.path.basename(folder), result.returncode))
    with open(log) as f:
        return f.read().splitlines()


def after_load(lines):
    """the game's state lines from the load on (the last 'state loaded' event; all of them when there is none)"""
    start = 0
    for i, line in enumerate(lines):
        if "event: state loaded" in line:
            start = i + 1
    return [line for line in lines[start:] if LINE.match(line)]


def from_vblank(lines, vblank):
    return [line for line in lines if LINE.match(line) and int(LINE.match(line).group(1)) >= vblank]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--keep", help="write the runs (logs, pictures, the state) into this folder")
    parser.add_argument("--renderer", default="gs", choices=("gs", "pc", "scene"))
    args = parser.parse_args()
    # never the player's data folder: settings, the automap's files and caches go to a temporary one
    os.environ.setdefault("KF4_DATA_DIR", tempfile.mkdtemp(prefix="kf4_data_"))
    if not args.disc:
        sys.exit("state_test: pass --disc or set KF4_DISC")
    exe = os.path.abspath(args.exe) if args.exe else find_exe()
    folder = args.keep or tempfile.mkdtemp(prefix="kf4_state_")
    state = os.path.join(folder, "test.kf4state")
    if os.path.exists(state):
        os.remove(state)

    saved = run(exe, args.disc, args.renderer, os.path.join(folder, "saved"),
                ["--teleport", TELEPORT, "--state-save-at", "%d:%s" % (SAVE_AT, state)])
    if not os.path.exists(state):
        sys.exit("state_test: no state was saved")
    reference = from_vblank(saved, SAVE_AT)
    failed = 0
    # (a picture is written once: the run loaded at a vblank wrote those before it from the other map)
    for name, extra, first in (("loaded at the start", ["--state-load", state], 0),
                               ("loaded at vblank %d" % LOAD_AT, ["--state-load", "%d:%s" % (LOAD_AT, state)], LOAD_AT)):
        sub = os.path.join(folder, name.replace(" ", "_"))
        lines = after_load(run(exe, args.disc, args.renderer, sub, extra))
        if not lines:
            print("FAIL %s: no game state lines after the load" % name)
            failed += 1
            continue
        if lines != reference[:len(lines)] or len(lines) < END - SAVE_AT:
            bad = next((i for i, (a, b) in enumerate(zip(lines, reference)) if a != b), min(len(lines), len(reference)))
            print("FAIL %s: the game goes otherwise from line %d of %d" % (name, bad, len(lines)))
            if bad < len(lines) and bad < len(reference):
                print("  saved:  %s\n  loaded: %s" % (reference[bad], lines[bad]))
            failed += 1
        else:
            print("ok   %s: %d vblanks as the saving run's" % (name, len(lines)))
        for n in (n for n in FRAMES if n > first):
            picture = "frame_%06d.png" % n
            a, b = os.path.join(folder, "saved", picture), os.path.join(sub, picture)
            if not os.path.exists(b) or not filecmp.cmp(a, b, shallow=False):
                print("FAIL %s: %s differs from the saving run's" % (name, picture))
                failed += 1
    if failed:
        sys.exit("state_test: %d check(s) failed (runs in %s)" % (failed, folder))
    print("state_test: passed")
    if not args.keep:
        shutil.rmtree(folder, ignore_errors=True)  # (the pictures and the state hold the game's data)


if __name__ == "__main__":
    main()
