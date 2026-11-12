#!/usr/bin/env python3
"""Two builds of the game, the same game: a release build (LTO, PGO) against the developers' build.

    tools/build_compare.py --exe A --exe B [--disc ISO] [--spots 0,9,9:2,63] [--frames 3000] [--keep DIR]
                           [-- extra program arguments]

Each spot plays tests/newgame.pad and teleports to MAP[:PLACE] at vblank 1900 (--teleport, --invincible), headless at
--frame-rate 240 with a fixed clock and no memory card, once with each program. A run writes the picture shown at a
few vblanks after the teleport (--dump-frames), the game's state line at every vblank (--log) and F10's state dump
(KF4_DEBUG_AT=state@) near the end. The two programs' pictures must be the same byte for byte, their state lines the
same, and their state dumps the same but for the dump's own header (the build, the time). A difference means the
builds compute something differently: the release build's flags changed a float's rounding or the order of the
game's maths (tools/README.md, "A release build").

One program runs at a time, each with a data folder of its own (KF4_DATA_DIR) under --keep (a temporary folder by
default, removed afterwards). Needs the disc (--disc or KF4_DISC) and a Vulkan GPU.
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")
TELEPORT = 1900
# the dump's lines that name the build or the moment, not the game
HEADER_KEYS = ("build", "time", "date", "host", "program", "exe", "data_dir")
# the dump's host code addresses (port/debug/game_dump.c's %p), compared by function name
CODE_POINTERS = ("handler",)


def run(exe, disc, spot, frames, folder, extra, timeout):
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    dumps = [TELEPORT + 300, TELEPORT + 600, frames - 1]
    log = os.path.join(folder, "state.log")
    cmd = [os.path.abspath(exe), "--disc", disc, "--headless", "--no-audio", "--memcard", "none", "--clock", "2026-01-01T12:00:00",
           "--input", PAD, "--teleport", "%d:%s" % (TELEPORT, spot), "--invincible", "--frame-rate", "240",
           "--frames", str(frames), "--dump-frames", ",".join(str(v) for v in dumps), "--dump-dir", folder,
           "--log", log] + extra
    env = dict(os.environ, KF4_DATA_DIR=folder, KF4_DEBUG_AT="state@%d" % (frames - 50))
    try:
        result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                timeout=timeout)
        out = result.stdout
    except subprocess.TimeoutExpired as e:
        # on Windows the program sometimes stays after --frames' exit has written everything
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        out += "\n(build_compare: killed after %d s)\n" % timeout
    with open(os.path.join(folder, "run.log"), "w") as f:
        f.write(out)
    return folder


def pictures(folder):
    return sorted(n for n in os.listdir(folder) if n.endswith(".png"))


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def state_lines(folder):
    """The log's state lines (a vblank, then the state), without what happens between (timings, threads)."""
    path = os.path.join(folder, "state.log")
    if not os.path.exists(path):
        return []
    with open(path, errors="replace") as f:
        return [line for line in f if line.split(" ", 1)[0].isdigit()]


class Symbols:
    """The program's functions by address (nm), to name the dump's code pointers: a build with another code layout
    (PGO) has the same game with its functions elsewhere"""

    def __init__(self, exe):
        nm = shutil.which("llvm-nm") or shutil.which("nm")
        out = subprocess.run([nm, exe], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout if nm else ""
        self.table = sorted((int(p[0], 16), p[2]) for p in (line.split() for line in out.splitlines())
                            if len(p) == 3 and p[1] in "tT" and re.fullmatch(r"[0-9a-fA-F]+", p[0]))
        self.addrs = [a for a, _ in self.table]

    def name(self, text):
        try:
            addr = int(text, 16)
        except ValueError:
            return text
        k = bisect.bisect_right(self.addrs, addr) - 1
        if k < 0:
            return text
        return "%s+0x%x" % (self.table[k][1], addr - self.table[k][0])


def name_code_pointers(data, symbols):
    if isinstance(data, dict):
        return {k: (symbols.name(v) if k in CODE_POINTERS and isinstance(v, str) else name_code_pointers(v, symbols))
                for k, v in data.items()}
    if isinstance(data, list):
        return [name_code_pointers(v, symbols) for v in data]
    return data


def state_dump(folder, symbols):
    captures = os.path.join(folder, "captures")
    if not os.path.isdir(captures):
        return None
    names = sorted(n for n in os.listdir(captures) if n.endswith(".json"))
    if not names:
        return None
    with open(os.path.join(captures, names[-1]), errors="replace") as f:
        text = f.read()
    try:
        data = json.loads(text)
    except ValueError:
        return text
    if isinstance(data, dict):
        for key in HEADER_KEYS:
            data.pop(key, None)
    data = name_code_pointers(data, symbols)
    return json.dumps(data, sort_keys=True, indent=0)


def first_difference(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    return min(len(a), len(b)), "(%d lines)" % len(a), "(%d lines)" % len(b)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", action="append", required=True, help="twice: the two programs")
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--spots", default="0,9,9:2,63", help="MAP[:PLACE] joined by commas")
    parser.add_argument("--frames", type=int, default=3000, help="vblanks each run plays")
    parser.add_argument("--timeout", type=int, default=600, help="seconds before a run is stopped")
    parser.add_argument("--keep", help="keep the runs here")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if len(args.exe) != 2:
        sys.exit("build_compare: --exe twice")
    if not args.disc:
        sys.exit("build_compare: --disc or KF4_DISC")
    keep = args.keep or tempfile.mkdtemp(prefix="kf4_build_compare_")
    symbols = [Symbols(exe) for exe in args.exe]
    failed = 0
    for spot in args.spots.split(","):
        spot = spot if ":" in spot else spot + ":0"
        folders = [run(exe, args.disc, spot, args.frames, os.path.join(keep, "%s_%d" % (spot.replace(":", "_"), i)),
                       args.extra, args.timeout) for i, exe in enumerate(args.exe)]
        problems = []
        pics = [pictures(f) for f in folders]
        if pics[0] != pics[1] or not pics[0]:
            problems.append("pictures written: %s against %s" % (pics[0], pics[1]))
        else:
            for name in pics[0]:
                if digest(os.path.join(folders[0], name)) != digest(os.path.join(folders[1], name)):
                    problems.append("picture %s differs" % name)
        lines = [state_lines(f) for f in folders]
        if not lines[0]:
            problems.append("no state lines logged")
        elif lines[0] != lines[1]:
            i, x, y = first_difference(*lines)
            problems.append("state line %d differs:\n    %s    %s" % (i, x, y))
        dumps = [state_dump(f, s) for f, s in zip(folders, symbols)]
        if dumps[0] is None or dumps[1] is None:
            problems.append("no state dump (F10) written")
        elif dumps[0] != dumps[1]:
            i, x, y = first_difference(dumps[0].splitlines(), dumps[1].splitlines())
            problems.append("state dump differs at line %d:\n    %s\n    %s" % (i, x, y))
        print("%-6s %s: %d pictures, %d state lines, state dump %s" % (
            spot, "SAME" if not problems else "DIFFERENT", len(pics[0]), len(lines[0]),
            "compared" if dumps[0] is not None else "missing"))
        for p in problems:
            print("    " + p)
        failed += bool(problems)
    if not args.keep:
        shutil.rmtree(keep, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
