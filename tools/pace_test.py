#!/usr/bin/env python3
"""How fast the game draws in play at a few places: the pictures a second a window would show.

    tools/pace_test.py [--disc ISO] [--exe PATH] [--maps 0,9] [--hz 170] [--screen-hz 169.83] [--frame-rate monitor]
                       [--runs N] [--keep DIR]
                       [-- extra program arguments]

Each run plays tests/newgame.pad and teleports to the map at vblank 1900 (--teleport), headless, with the owner's
picture settings (4x with native widescreen, anisotropic 16, cut-out AA, native draw: OWNER below) at --hz.

pace: real time with KF4_PRESENT_SIM=HZ (the Vulkan presenter's pacing without a window): the pictures handed over
      each second of play (KF4_PRESENT_LOG), the game's vblanks shown each second (60: the game keeps its speed),
      the screen's refreshes in play whose picture is not one refresh on from the last one's (a jump: --screen-hz
      is the simulated screen's own rate, 169.83 where the system names 170),
      the vblanks cut to fewer pictures between, and the GPU's time a picture (KF4_GSN_GPU_TIME=1). With
      KF4_INTERP_CUT=0 in the environment nothing is cut: what the place keeps up, the game slowed where it does not.
Every run uses a data folder of its own (KF4_DATA_DIR) with the pipeline cache of the owner's, when there is one, so
that no run compiles pipelines while it is timed. Run one at a time, with the GPU otherwise idle.
"""
import argparse
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")
TELEPORT = 1900
OWNER = ["--native-draw", "on", "--native-scale", "4", "--widescreen", "--native-widescreen",
         "--native-msaa", "1", "--native-aniso", "16", "--native-cutout-aa", "ssaa", "--present-scaling", "best",
         "--present-sharpen", "1"]


def find_exe():
    path = os.path.join(ROOT, "out", "linux-clang", "kf4")
    if not os.path.exists(path):
        sys.exit("pace_test: no program at %s; build it or pass --exe" % path)
    return path


def owner_data_dir():
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA", ""), "holyland")
    return os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "holyland")


def data_folder(keep, name):
    folder = os.path.join(keep, name)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    src = os.path.join(owner_data_dir(), "native_gs_pipelines.bin")
    if os.path.exists(src):
        shutil.copy(src, folder)
    return folder


def command(args, folder, map_index, frames, flags):
    """A run's command line (args.exe, .disc, .hz, .extra; tools/sampler/profile.py's too). It dumps the last frame:
    a --headless run that dumps none starts none of the GS's threads, and draws nothing."""
    return [args.exe, "--disc", args.disc, "--headless", "--no-audio", "--memcard", "none", "--clock",
            "2026-01-01T12:00:00", "--input", PAD, "--teleport", "%d:%d:0" % (TELEPORT, map_index), "--frames",
            str(frames), "--stats", "--dump-frames", str(frames - 1), "--dump-dir", folder, "--frame-rate",
            args.frame_rate or str(args.hz)] + flags + OWNER + args.extra


def run(args, name, map_index, frames, env_extra, flags):
    folder = data_folder(args.keep, name)
    env = dict(os.environ, KF4_DATA_DIR=folder, **env_extra)
    cmd = command(args, folder, map_index, frames, flags)
    result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with open(os.path.join(folder, "run.log"), "w") as f:
        f.write(result.stdout)
    if result.returncode != 0:
        sys.stdout.write(result.stdout[-4000:])
        sys.exit("pace_test: %s exited with %d" % (name, result.returncode))
    return folder, result.stdout


def number(pattern, text, default=0.0):
    m = re.search(pattern, text)
    return float(m.group(1)) if m else default


def pace(args, map_index, n):
    frames = TELEPORT + 400 + args.seconds * 60
    folder, out = run(args, "pace_%03d_%d" % (map_index, n), map_index, frames,
                      {"KF4_PRESENT_SIM": str(args.screen_hz or args.hz), "KF4_GSN_GPU_TIME": "1",
                       "KF4_PRESENT_LOG": os.path.join(args.keep, "pace_%03d_%d.present" % (map_index, n))}, [])
    seconds = {}
    vblanks = {}
    screen = None
    shown = {}  # each refresh of the screen in play: the time of the last picture presented before it
    with open(os.path.join(args.keep, "pace_%03d_%d.present" % (map_index, n))) as f:
        for line in f:
            p = line.split()
            if p[0] == "R":
                screen = (float(p[1]), float(p[2]))
                continue
            # (pictures shown after the teleport's load: 400 vblanks on; not the last vblank's, whose pictures are
            # written out as the run's --dump-frames, 0.4 s of this thread: it was every run's lowest second)
            if p[0] == "S" or int(p[1]) < TELEPORT + 400 or int(p[1]) >= frames - 1:
                continue
            s = int(float(p[0]))
            seconds[s] = seconds.get(s, 0) + 1
            vblanks.setdefault(s, set()).add(int(p[1]))
            if screen:
                shown[math.ceil((float(p[0]) - screen[0]) * screen[1])] = int(p[1]) + float(p[2])
    whole = [seconds[s] for s in sorted(seconds)][1:-1]  # (the first and last seconds are partial)
    game = [len(vblanks[s]) for s in sorted(vblanks)][1:-1]  # (the game's vblanks shown each second: 60 keeps up)
    return {
        "uneven refreshes a second": uneven_refreshes(shown, screen) * 60.0 / (frames - TELEPORT - 400),
        "pictures a second": statistics.mean(whole) if whole else 0.0,
        "lowest second": min(whole) if whole else 0,
        "game vblanks a second": statistics.mean(game) if game else 0.0,
        "vblanks with none between": number(r"\((\d+) with none\)", out),
        "GPU ms a picture": number(r"GPU time: [\d.]+ ms in all, ([\d.]+) ms a picture", out),
    }


def uneven_refreshes(shown, screen):
    """The refreshes whose picture is not one refresh after the one before's (to half a refresh): the world standing
    still for a refresh, or moving two. shown: refresh -> the picture's time in vblanks; a refresh missing from it
    shows the picture before again."""
    if not shown or not screen:
        return 0
    refresh = 1.0 / screen[1]
    vblank = 1001.0 / 60000.0
    uneven = 0
    first, last = min(shown), max(shown)
    before = shown[first]
    for k in range(first + 1, last + 1):
        now = shown.get(k, before)
        uneven += abs((now - before) * vblank - refresh) > 0.5 * refresh
        before = now
    return uneven



def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--maps", default="0,9")
    parser.add_argument("--hz", type=int, default=170)
    parser.add_argument("--screen-hz", type=float, help="the simulated screen's own rate (default: --hz)")
    parser.add_argument("--frame-rate", help="--frame-rate (default: --hz; monitor: the screen's, by its refresh clock)")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--seconds", type=int, default=25, help="seconds of play measured (at 60 vblanks a second)")
    parser.add_argument("--keep")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("pace_test: pass --disc or set KF4_DISC")
    args.exe = args.exe or find_exe()
    # (without --keep, a temporary folder deleted at the end: each run's data folder is about 400 MB, and /tmp is small)
    temporary = not args.keep
    args.keep = args.keep or tempfile.mkdtemp(prefix="kf4_pace_")
    os.makedirs(args.keep, exist_ok=True)
    try:
        for map_index in [int(m) for m in args.maps.split(",")]:
            for n in range(args.runs):
                result = pace(args, map_index, n)
                print("map %03d run %d: %s" % (map_index, n, ", ".join(
                    "%s %.2f" % (k, v) if isinstance(v, float) else "%s %s" % (k, v) for k, v in result.items())),
                    flush=True)
    finally:
        if temporary:
            shutil.rmtree(args.keep, ignore_errors=True)
    if not temporary:
        print("pace_test: runs kept in %s" % args.keep)


if __name__ == "__main__":
    main()
