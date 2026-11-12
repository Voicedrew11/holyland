#!/usr/bin/env python3
"""Profile the game in play at a place: every thread's functions, then what the busiest (or --thread) spends its time
in, and on Linux the GPU's time a picture by kind of work (docs/port/PROFILING.md). On Windows each thread of kf4.exe
is sampled from outside with kf4-sampler (tools/sampler/sampler.c); on Linux the game samples itself (--profile-at,
port/platform/sampler.c), and --gpu-detail also times the PC renderer's draws by shader state.

    python tools/sampler/profile.py [--disc ISO] [--exe PATH] [--map 0] [--seconds 15] [--hz 170]
                                    [--interval-us 500] [--thread FUNC|TID|NAME] [--top 40] [--keep DIR]
                                    [--gpu-detail]
                                    [-- extra program arguments]

The run is tools/pace_test.py's pace run, with its settings (shared, not copied): tests/newgame.pad, the teleport
to the map at vblank 1900, headless, the owner's picture settings, real time with KF4_PRESENT_SIM=HZ, a data folder
of its own (KF4_DATA_DIR) with the owner's pipeline cache. KF4_INTERP_CUT is 0 unless the environment sets it: every
picture between is drawn, the GS's thread works flat out. Sampling starts when a picture of the game's vblank 2300
is shown (KF4_PRESENT_LOG: after the teleport's load, where pace_test starts counting) and lasts --seconds, every
--interval-us. Then every thread (stacks.py --threads) and --thread's functions by inclusive and self share
(stacks.py: on Windows the GS's thread, ring_thread, by default; on Linux the busiest). Extra program arguments go
to the game after the owner's settings: -- --renderer pc profiles the PC renderer.

Kept in --keep (a temporary folder by default): samples.txt, which stacks.py reads again (--callers, --callees,
another --thread), the game's output (run.log) and its data folder. The program wants a RelWithDebInfo build
(llvm-symbolizer names the frames from its DWARF); on Windows with kf4-sampler.exe beside it (built with it) and
llvm-mingw's bin on PATH, on Linux with frame pointers (-DKF4_FRAME_POINTERS=ON, the default). Run alone, with the GPU
otherwise idle.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import pace_test  # noqa: E402  (the owner's settings and the run's command)
import stacks  # noqa: E402

AFTER_LOAD = pace_test.TELEPORT + 400


def wait_for_picture(game, path, vblank):
    """Until the game shows a picture of the vblank or later; False if it exits first. The present log is the
    game's stdio, written out a few KB at a time (up to a second late): whole lines only."""
    f = None
    rest = ""
    try:
        while game.poll() is None:
            if f is None and os.path.exists(path):
                f = open(path)
            if f is not None:
                lines = (rest + f.read()).split("\n")
                rest = lines.pop()
                for line in lines:
                    p = line.split()
                    if len(p) >= 2 and p[0] not in ("S", "R") and int(p[1]) >= vblank:  # (R: the rate line)
                        return True
            time.sleep(0.2)
        return False
    finally:
        if f is not None:
            f.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--exe")
    parser.add_argument("--sampler", help="kf4-sampler.exe (by default the one beside --exe)")
    parser.add_argument("--map", type=int, default=0)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--hz", type=int, default=170)
    parser.add_argument("--interval-us", type=int, default=500)
    parser.add_argument("--thread", help="a function, a thread id or (Linux) a thread's name (stacks.py)")
    parser.add_argument("--gpu-detail", action="store_true", help="(Linux) the PC renderer's GPU time by shader state")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--keep")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("profile: pass --disc or set KF4_DISC")
    args.exe = args.exe or pace_test.find_exe()
    args.frame_rate = None
    if os.name != "nt":
        linux(args)
        return
    args.thread = args.thread or "ring_thread"
    sampler = args.sampler or os.path.join(os.path.dirname(os.path.abspath(args.exe)), "kf4-sampler.exe")
    if not os.path.exists(sampler):
        sys.exit("profile: no %s: cmake --build out/windows-clang --target kf4-sampler" % sampler)
    args.keep = args.keep or tempfile.mkdtemp(prefix="kf4_prof_")
    os.makedirs(args.keep, exist_ok=True)
    folder = pace_test.data_folder(args.keep, "data")
    present = os.path.join(args.keep, "present.txt")
    samples = os.path.join(args.keep, "samples.txt")
    frames = AFTER_LOAD + int((args.seconds + 5) * 60)
    env = dict(os.environ, KF4_DATA_DIR=folder, KF4_PRESENT_SIM=str(args.hz), KF4_PRESENT_LOG=present,
               KF4_INTERP_CUT=os.environ.get("KF4_INTERP_CUT", "0"))
    with open(os.path.join(args.keep, "run.log"), "w") as log:
        game = subprocess.Popen(pace_test.command(args, folder, args.map, frames, []), env=env, stdout=log,
                                stderr=subprocess.STDOUT)
        if not wait_for_picture(game, present, AFTER_LOAD):
            sys.exit("profile: the game exited (%d) before vblank %d: %s" % (game.returncode, AFTER_LOAD,
                                                                             os.path.join(args.keep, "run.log")))
        result = subprocess.run([sampler, str(game.pid), str(args.seconds), str(args.interval_us), samples],
                                capture_output=True, text=True)
        print(result.stderr.strip(), flush=True)
        if result.returncode != 0:
            game.kill()
            sys.exit("profile: kf4-sampler exited with %d" % result.returncode)
        if game.wait() != 0:
            print("profile: the game exited with %d (run.log)" % game.returncode)
    stacks.report(samples, args.exe, thread=args.thread, threads=True, top=args.top)
    print("profile: kept in %s (python tools/sampler/stacks.py %s %s --callers FUNC)" % (args.keep, samples, args.exe))


def linux(args):
    """The game samples itself: --profile-at from AFTER_LOAD for --seconds, then the report"""
    args.keep = args.keep or tempfile.mkdtemp(prefix="kf4_prof_")
    os.makedirs(args.keep, exist_ok=True)
    folder = pace_test.data_folder(args.keep, "data")
    samples = os.path.join(args.keep, "samples.txt")
    frames = AFTER_LOAD + int((args.seconds + 2) * 60)
    env = dict(os.environ, KF4_DATA_DIR=folder, KF4_PRESENT_SIM=str(args.hz),
               KF4_INTERP_CUT=os.environ.get("KF4_INTERP_CUT", "0"), KF4_GPU_DETAIL="1" if args.gpu_detail else "0")
    args.extra = args.extra + ["--profile-at", "%d:%d:%s" % (AFTER_LOAD, max(1, round(args.seconds)), samples)]
    with open(os.path.join(args.keep, "run.log"), "w") as log:
        code = subprocess.run(pace_test.command(args, folder, args.map, frames, []), env=env, stdout=log,
                              stderr=subprocess.STDOUT).returncode
    if code != 0:
        print("profile: the game exited with %d (%s)" % (code, os.path.join(args.keep, "run.log")))
    if not os.path.exists(samples):
        sys.exit("profile: no samples written (%s)" % os.path.join(args.keep, "run.log"))
    stacks.report(samples, args.exe, thread=args.thread, threads=True, top=args.top)
    print()
    stacks.report(samples, args.exe, top=args.top, gpu=True)
    print("profile: kept in %s (python3 tools/sampler/stacks.py %s %s --callers FUNC)" % (args.keep, samples, args.exe))


if __name__ == "__main__":
    main()
