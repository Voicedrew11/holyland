#!/usr/bin/env python3
"""A release build with profile-guided optimisation (PGO): build, train, build again.

    tools/pgo_train.py --preset windows-clang-release [--disc ISO] [--spots 0,9,9:2,63] [--frames 3000]
                       [--build DIR] [--jobs N] [-D VAR=VALUE ...] [-- extra program arguments]

1. Configures the preset in out/<preset>-pgo (--build) with KF4_PGO=generate and builds kf4.
2. Trains: plays tests/newgame.pad and teleports to each spot at vblank 1900, headless and silent (the sound is still
   made), at --frame-rate 240 with the PC renderer drawing every vblank, for --frames vblanks; one run at a time,
   each with a data folder of its own (KF4_DATA_DIR). The program writes its profile as it stops (kernel.c).
3. clang: merges the runs' .profraw files into <build>/pgo/kf4.profdata with llvm-profdata (the one beside the
   compiler, or llvm-profdata-<version> on PATH). gcc: the runs added to the .gcda files beside the objects.
4. Configures the same folder with KF4_PGO=<that file> (clang) or KF4_PGO=use (gcc) and builds everything.

-D passes cache variables to both configures (on Windows SDL2_DIR, for one). Afterwards check the build against the
developers' one with tools/build_compare.py. The spots are the ones the frames are checked at; the profile is only
as good as the play it saw, so a run that never opens the menus leaves the menus' code as cold.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAD = os.path.join(ROOT, "tests", "newgame.pad")
TELEPORT = 1900


def call(cmd, **kw):
    print("pgo_train: " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def configure(args, value):
    call(["cmake", "--preset", args.preset, "-B", args.build, "-DKF4_PGO=" + value] +
         ["-D" + d for d in args.define], cwd=ROOT)


def cache_value(build, name):
    with open(os.path.join(build, "CMakeCache.txt")) as f:
        for line in f:
            if line.startswith(name + ":"):
                return line.split("=", 1)[1].strip()
    return ""


def find_profdata(build):
    compiler = cache_value(build, "CMAKE_C_COMPILER")
    names = ["llvm-profdata"]
    version = cache_value(build, "CMAKE_C_COMPILER_VERSION")
    if not version:
        out = subprocess.run([compiler, "--version"], stdout=subprocess.PIPE, text=True).stdout
        m = re.search(r"version (\d+)", out)
        version = m.group(1) if m else ""
    if version:
        names.append("llvm-profdata-" + version.split(".")[0])
    folder = os.path.dirname(compiler)
    for name in names:
        for candidate in (os.path.join(folder, name), os.path.join(folder, name + ".exe")):
            if os.path.exists(candidate):
                return candidate
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    sys.exit("pgo_train: no llvm-profdata beside %s or on PATH" % compiler)


def train(args, exe, profile_dir, clang):
    for spot in args.spots.split(","):
        spot = spot if ":" in spot else spot + ":0"
        folder = os.path.join(profile_dir, "run_" + spot.replace(":", "_"))
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(folder)
        cmd = [exe, "--disc", args.disc, "--headless", "--no-audio", "--memcard", "none", "--clock",
               "2026-01-01T12:00:00", "--input", PAD, "--teleport", "%d:%s" % (TELEPORT, spot), "--invincible",
               "--frame-rate", "240", "--frames", str(args.frames), "--dump-frames", str(args.frames - 1),
               "--dump-dir", folder] + args.extra
        env = dict(os.environ, KF4_DATA_DIR=folder)
        if clang:
            env["LLVM_PROFILE_FILE"] = os.path.join(profile_dir, "kf4-%p.profraw")
        print("pgo_train: training at %s" % spot, flush=True)
        try:
            result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    timeout=args.timeout)
            log = result.stdout
        except subprocess.TimeoutExpired as e:
            # on Windows the program sometimes stays after --frames' exit; the profile is written before it
            log = e.stdout.decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
            log += "\n(pgo_train: stopped after %d s)\n" % args.timeout
        with open(os.path.join(folder, "run.log"), "w") as f:
            f.write(log)
        shutil.rmtree(os.path.join(folder, "captures"), ignore_errors=True)
        for png in glob.glob(os.path.join(folder, "*.png")):
            os.remove(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--preset", required=True, help="a release preset (CMakePresets.json)")
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--spots", default="0,9,9:2,63", help="MAP[:PLACE] joined by commas")
    parser.add_argument("--frames", type=int, default=3000, help="vblanks each training run plays")
    parser.add_argument("--timeout", type=int, default=900, help="seconds before a training run is stopped")
    parser.add_argument("--build", help="the build folder (default out/<preset>-pgo)")
    parser.add_argument("--jobs", type=int, help="cmake --build -j")
    parser.add_argument("-D", dest="define", action="append", default=[], help="VAR=VALUE for both configures")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if not args.disc:
        sys.exit("pgo_train: --disc or KF4_DISC")
    args.build = os.path.abspath(args.build or os.path.join(ROOT, "out", args.preset + "-pgo"))
    jobs = ["-j", str(args.jobs)] if args.jobs else []

    configure(args, "generate")
    clang = cache_value(args.build, "CMAKE_C_COMPILER_ID") == "Clang" or "clang" in os.path.basename(
        cache_value(args.build, "CMAKE_C_COMPILER"))
    profile_dir = os.path.join(args.build, "pgo")
    shutil.rmtree(profile_dir, ignore_errors=True)
    os.makedirs(profile_dir)
    if not clang:
        for gcda in glob.glob(os.path.join(args.build, "**", "*.gcda"), recursive=True):
            os.remove(gcda)
    call(["cmake", "--build", args.build, "--target", "kf4"] + jobs)
    exe = os.path.join(args.build, "kf4.exe" if os.name == "nt" else "kf4")
    train(args, exe, profile_dir, clang)

    if clang:
        raws = glob.glob(os.path.join(profile_dir, "*.profraw"))
        if not raws:
            sys.exit("pgo_train: the training runs wrote no profile")
        profdata = os.path.join(profile_dir, "kf4.profdata")
        call([find_profdata(args.build), "merge", "-o", profdata] + raws)
        for raw in raws:
            os.remove(raw)
        configure(args, profdata)
    else:
        if not glob.glob(os.path.join(args.build, "**", "*.gcda"), recursive=True):
            sys.exit("pgo_train: the training runs wrote no .gcda files")
        configure(args, "use")
    call(["cmake", "--build", args.build] + jobs)
    print("pgo_train: built %s" % exe)


if __name__ == "__main__":
    main()
