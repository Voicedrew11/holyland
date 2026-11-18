#!/usr/bin/env python3
"""Record a session as it is played, and replay it headless: for finding and bisecting what a player saw.

    tools/record_input.py record [--disc ISO] [--exe PATH] [--out DIR] [--quick FILE|none] [--clock TIME] [-- kf4 options]
    tools/record_input.py replay REC [--disc ISO] [--exe PATH] [--out DIR] [--check] [--at VBLANK,...] [-- kf4 options]

record: copies what the run starts from out of the data folder (settings.json, overlay.ini, quick saves, automap,
hands, memory card) into REC/start, then starts the game in its window with the player's settings, a fixed
clock, the newest quick save (loaded by itself) and --record-input REC/input.pad. Play as usual; the recording is
complete when the game is closed. REC is <data folder>/captures/input-YYYYMMDD-HHMMSS by default, beside F8's and
F9's captures (the data folder: KF4_DATA_DIR, else %APPDATA%\\holyland or ~/.config/holyland). REC/info.txt names the
program, the quick save, the clock and the options.

replay: plays REC/input.pad again headless and silent (--no-audio), with KF4_DATA_DIR at a copy of REC/start
(OUT/data) and KF4_READ_SETTINGS=1, the same quick save and clock, at --frame-rate 240, to 60 vblanks after the
recording ends. It prints the vblanks the player pressed F8 at, and fails if the replay says it went out of step.
--at dumps the pictures of those vblanks (default: the F8 vblanks) into OUT. --check replays twice and compares the
per-frame logs and the dumped pictures byte for byte. --exe replays with another build (bisecting). Options after
-- go to the program (e.g. --gs-capture-at 3616, --log FILE).

What is replayed and what not: tools/README.md, "record_input.py". The disc comes from --disc or KF4_DISC; the
program from --exe (record: KF4_EXE too), or the first of out/linux-clang, out/linux-gcc and out/windows-clang that
exists. A recording holds the player's saves and settings: keep it out of the repository.
"""
import argparse
import datetime
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLOCK = "2026-01-01T12:00:00"
START_FILES = ("settings.json", "overlay.ini")
START_DIRS = ("quicksaves", "automap", "hands", "weapon_sets", "memcard")
PIPELINES = os.path.join(tempfile.gettempdir(), "kf4_replay_pipelines")
PIPELINE_FILES = ("pc_pipelines.bin", "pc_pipeline_list.bin")
OUT_OF_STEP = "[pad] the replay is out of step"


def find_exe():
    for preset in ("linux-clang", "linux-gcc", "windows-clang"):
        for name in ("kf4", "kf4.exe"):
            path = os.path.join(ROOT, "out", preset, name)
            if os.path.exists(path):
                return path
    sys.exit("record_input: no built program under out/; build one or pass --exe")


def data_dir():
    """The program's data folder, as port_data_dir() (port/overlay/settings.c) works it out."""
    if os.environ.get("KF4_DATA_DIR"):
        return os.environ["KF4_DATA_DIR"]
    if os.name == "nt" and os.environ.get("APPDATA"):
        return os.path.join(os.environ["APPDATA"], "holyland")
    if os.environ.get("XDG_CONFIG_HOME"):
        return os.path.join(os.environ["XDG_CONFIG_HOME"], "holyland")
    return os.path.join(os.path.expanduser("~"), ".config", "holyland")


def read_info(rec):
    info = {}
    path = os.path.join(rec, "info.txt")
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                key, sep, value = line.rstrip("\r\n").partition("=")
                if sep:
                    info[key.strip()] = value.strip()
    return info


def pad_summary(pad):
    """The vblank the recording ends at, and the vblanks F8 was pressed at."""
    end, f8 = 0, []
    with open(pad) as f:
        for line in f:
            m = re.match(r"#\s*vblank (\d+): F8", line)
            if m:
                f8.append(int(m.group(1)))
            words = line.split("#", 1)[0].split()
            if not words or not words[0].isdigit():
                continue
            start = int(words[0])
            if len(words) > 1 and words[1] in ("look", "event"):
                end = max(end, start + 1)
                continue
            # <vblank> <buttons|stick=..> ... [<vblanks>]: 6 vblanks when the count is left out
            last = words[-1]
            length = int(last) if len(words) > 1 and last.isdigit() else 6
            end = max(end, start + length)
    return end, f8


def record(args, extra):
    exe = os.path.abspath(args.exe or os.environ.get("KF4_EXE") or find_exe())
    data = data_dir()
    rec = args.out or os.path.join(data, "captures", "input-" + datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    start = os.path.join(rec, "start")
    os.makedirs(start, exist_ok=True)

    quick = args.quick
    if quick is None:
        saves = os.path.join(data, "quicksaves")
        found = sorted((os.path.join(saves, n) for n in os.listdir(saves) if n.endswith(".kf4quick")),
                       key=os.path.getmtime, reverse=True) if os.path.isdir(saves) else []
        if not found:
            sys.exit("record_input: no quick save in %s (pass --quick FILE, or --quick none to start at the title)" % saves)
        quick = found[0]

    for name in START_FILES:
        if os.path.exists(os.path.join(data, name)):
            shutil.copy2(os.path.join(data, name), start)
    for name in START_DIRS:
        if os.path.isdir(os.path.join(data, name)):
            shutil.copytree(os.path.join(data, name), os.path.join(start, name), dirs_exist_ok=True)
    quick_copy = None
    if quick != "none":
        os.makedirs(os.path.join(start, "quicksaves"), exist_ok=True)
        quick_copy = os.path.join(start, "quicksaves", os.path.basename(quick))
        if not os.path.exists(quick_copy):
            shutil.copy2(quick, quick_copy)

    with open(os.path.join(rec, "info.txt"), "w") as f:
        f.write("exe=%s\n" % os.path.abspath(exe))
        f.write("quick=%s\n" % (os.path.basename(quick) if quick_copy else "none"))
        f.write("clock=%s\n" % args.clock)
        f.write("started=%s\n" % datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        f.write("args=%s\n" % " ".join(extra))

    cmd = [exe, "--clock", args.clock, "--record-input", os.path.join(rec, "input.pad")]
    if args.disc:
        cmd[1:1] = ["--disc", args.disc]
    if quick_copy:
        cmd += ["--quick-load", quick_copy]
    print("recording to %s" % rec)
    if quick_copy:
        print("the quick save %s loads by itself: do not press the quick load key" % os.path.basename(quick))
    code = subprocess.call(cmd + extra)
    print("recording in %s (the program exited with %d)" % (rec, code))
    return code


def replay_once(exe, disc, rec, info, out, frames, frame_rate, at, extra):
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    data = os.path.join(out, "data")
    shutil.copytree(os.path.join(rec, "start"), data)
    # the compiled pipelines kept from replay to replay (they change no picture): a first run compiles them all as
    # it goes, and a compile that stalls the renderer for seconds can set off its GPU hang watch
    os.makedirs(PIPELINES, exist_ok=True)
    for name in PIPELINE_FILES:
        if os.path.exists(os.path.join(PIPELINES, name)):
            shutil.copy2(os.path.join(PIPELINES, name), data)
    env = dict(os.environ, KF4_READ_SETTINGS="1", KF4_DATA_DIR=os.path.abspath(os.path.join(out, "data")))
    cmd = [exe, "--disc", disc, "--no-audio", "--headless", "--frames", str(frames),
           "--clock", info.get("clock", CLOCK), "--input", os.path.join(rec, "input.pad"),
           "--log", os.path.join(out, "log.txt")]
    if frame_rate:
        cmd += ["--frame-rate", frame_rate]
    quick = info.get("quick", "none")
    if quick != "none":
        cmd += ["--quick-load", os.path.join(out, "data", "quicksaves", quick)]
    if at:
        cmd += ["--dump-frames", ",".join(str(v) for v in at), "--dump-dir", out]
    result = subprocess.run(cmd + extra, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            errors="replace")
    with open(os.path.join(out, "stdout.txt"), "w") as f:
        f.write(result.stdout)
    for name in PIPELINE_FILES:
        if os.path.exists(os.path.join(data, name)):
            shutil.copy2(os.path.join(data, name), PIPELINES)
    steps = [line for line in result.stdout.splitlines() if OUT_OF_STEP in line]
    return result.returncode, steps


def read_log(folder):
    """A replay's log, its own folder's name taken out (the memory card's and the quick save's paths)."""
    with open(os.path.join(folder, "log.txt"), errors="replace") as f:
        text = f.read().replace("\\", "/")
    return text.replace(os.path.abspath(folder).replace("\\", "/"), "<run>").splitlines()


def maps_seen(log):
    """The maps the player stood in, in order (the log's map= field)."""
    seq = []
    with open(log, errors="replace") as f:
        for line in f:
            m = re.search(r" map=([0-9A-Fa-f]+:[0-9A-Fa-f]+)", line)
            if m and (not seq or seq[-1] != m.group(1)):
                seq.append(m.group(1))
    return seq


def replay(args, extra):
    rec = args.rec
    info = read_info(rec)
    pad = os.path.join(rec, "input.pad")
    if not os.path.exists(pad):
        sys.exit("record_input: no input.pad in %s" % rec)
    disc = args.disc or os.environ.get("KF4_DISC")
    if not disc:
        sys.exit("record_input: pass --disc or set KF4_DISC")
    exe = os.path.abspath(args.exe) if args.exe else find_exe()
    end, f8 = pad_summary(pad)
    frames = args.frames or end + 60
    at = [int(v) for v in args.at.split(",") if v] if args.at is not None else f8
    print("recording ends at vblank %d; F8 at %s" % (end, " ".join(map(str, f8)) or "none"))
    out = args.out or tempfile.mkdtemp(prefix="kf4_replay_")

    runs = ["a", "b"] if args.check else [""]
    ok = True
    for name in runs:
        folder = os.path.join(out, name) if name else out
        code, steps = replay_once(exe, disc, rec, info, folder, frames, args.frame_rate, at, extra)
        print("replay%s: exit %d, maps %s" % (" " + name if name else "", code,
                                             " -> ".join(maps_seen(os.path.join(folder, "log.txt")))))
        if code == 70:
            print("  the renderer's GPU hang watch stopped it (data/gpu_hang_*.txt): replay again")
        for line in steps[:5]:
            print("  " + line)
        if code != 0 or steps:
            ok = False
    if args.check:
        a, b = os.path.join(out, "a"), os.path.join(out, "b")
        log_a, log_b = read_log(a), read_log(b)
        if log_a == log_b:
            print("logs: the same, %d lines" % len(log_a))
        else:
            ok = False
            print("logs: differ (%d lines against %d)" % (len(log_a), len(log_b)))
        pictures = sorted(n for n in os.listdir(a) if n.endswith(".png"))
        for n in pictures:
            same = os.path.exists(os.path.join(b, n)) and filecmp.cmp(os.path.join(a, n), os.path.join(b, n), False)
            ok = ok and same
            print("%s: %s" % (n, "byte for byte the same" if same else "DIFFERS"))
        if at and not pictures:
            ok = False
            print("no pictures were dumped")
    print("output in %s" % out)
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("record", help="play in a window while the input is recorded")
    r.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    r.add_argument("--exe")
    r.add_argument("--out", help="the recording's folder (default <data folder>/captures/input-DATE-TIME)")
    r.add_argument("--quick", help="the quick save to start from (default the newest; none: the title)")
    r.add_argument("--clock", default=CLOCK, help="the game's clock (default %s)" % CLOCK)
    p = sub.add_parser("replay", help="replay a recording headless")
    p.add_argument("rec", help="the recording's folder (input.pad, info.txt, start/)")
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--exe", help="the program (default the build under out/, not the one info.txt names)")
    p.add_argument("--out", help="where the replay's data copy, log and pictures go (default a temporary folder)")
    p.add_argument("--frames", type=int, help="vblanks to run (default the recording's end + 60)")
    p.add_argument("--frame-rate", default="240", help="--frame-rate for the program (default 240; '' for none)")
    p.add_argument("--at", help="vblanks to dump pictures at (default the F8 vblanks; '' for none)")
    p.add_argument("--check", action="store_true", help="replay twice and compare the logs and pictures")
    args = parser.parse_args(argv)
    sys.exit(record(args, extra) if args.command == "record" else replay(args, extra))


if __name__ == "__main__":
    main()
