#!/usr/bin/env python3
"""The CPU time each thread spends in play, for several builds of the game, alternated: what a release build saves.

    tools/build_cost.py --exe A --exe B [--exe C ...] [--disc ISO] [--map 9] [--runs 3] [--seconds 15] [--hz 240]
                        [--interval-us 5000] [--keep DIR] [-- extra program arguments]

Each run is tools/sampler/profile.py's (pace_test.py's run: tests/newgame.pad, the teleport at vblank 1900, headless,
the owner's picture settings, real time with KF4_PRESENT_SIM=HZ, every picture between drawn): each thread's CPU time
over --seconds of play, from the system's per-thread clock (Windows: kf4-sampler's GetThreadTimes; Linux: the game's
own sampler). The builds take turns, A B C, B C A, C A B..., so a change in the machine's load falls on all of them.

Prints, per thread (named by Linux's thread names, on Windows by the outermost function the thread runs), each
build's mean CPU ms a second of play (1000 = a whole core) with the spread of its runs, then the part of it not spent
spinning: the game's thread waits for DMA by spinning (port_dmac_wait, cpu_relax), 80% of its CPU time at 009 at
240, and that is no work. The spin's share comes from the samples (each thread's stacks), so the second number is
an estimate: a sampling interval of 5 ms (the default) over 15 s gives about 3000 samples a thread. Divide by 60 for
ms a vblank (the game keeps 60 vblanks a second at these rates; a vblank is a game frame at 60, half of one at 30).
--again reports kept runs again without running anything.

CPU time is less disturbed by other programs than pictures a second (pace_test.py), but not untouched: a busy machine
shares caches and memory and moves threads between cores. Measure on a quiet machine, the GPU otherwise idle.
"""
import argparse
import collections
import os
import re
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROFILE = os.path.join(ROOT, "tools", "sampler", "profile.py")
sys.path.insert(0, os.path.join(ROOT, "tools", "sampler"))
import stacks  # noqa: E402  (a profile run's file)

# frames that say nothing about which thread it is: a thread's lambda, libc++'s and the C runtime's wrappers
WRAPPERS = re.compile(r"^(operator\(\)|(void\*? )?std::|__|\?\?)")
# Windows: the threads by the function they start in, under Linux's names for them (PROFILING.md)
WINDOWS_NAMES = (("port_fiber_trampoline", "kf4"), ("Renderer::open", "kf4-pc"), ("Pump::start", "kf4-pc-pump"),
                 ("dmac_thread", "kf4-dma"), ("between_main", "kf4-between"))


# a thread waiting by spinning (port/platform: cpu_relax in a loop) is busy to the system's clock: those samples are
# counted apart, as the thread's spin
SPIN = ("cpu_relax",)


def analyse(prof, exe):
    """Each thread's name and the share of its busy samples spent spinning. Names: Linux's own; on Windows (no names)
    the outermost of the program's functions the thread runs, with its class (the PC renderer's two threads are both
    a run() in a lambda), under Linux's name for it where known."""
    addrs = set()
    for tid, rows in prof.stacks.items():
        for n, f in rows:
            addrs.update(int(a, 16) - (1 if i else 0) for i, a in enumerate(f) if a.startswith("0x"))
    addrs = sorted(addrs)
    out = subprocess.run(["llvm-symbolizer", "--obj=" + exe, "--functions=linkage", "--demangle"],
                         input="".join("0x%x\n" % a for a in addrs), capture_output=True, text=True).stdout
    sym = {a: block.split("\n")[0::2] for a, block in zip(addrs, out.strip("\n").split("\n\n"))}
    result = {}
    for tid, rows in prof.stacks.items():
        votes = collections.Counter()
        busy = spin = 0
        for n, f in rows:
            if f[0].rpartition("+")[0].lower() in stacks.IDLE:
                continue  # (waiting in the system: not CPU time)
            names = []
            for i, a in enumerate(f):
                if a.startswith("0x"):
                    names.extend(sym.get(int(a, 16) - (1 if i else 0), ["?"]))
            busy += n
            if any(x.startswith(SPIN) for x in names):
                spin += n
            own = [x for x in names if not WRAPPERS.match(x)]
            if own:
                votes[own[-1]] += n
        name = prof.names.get(tid)
        if not name and votes:
            name = re.sub(r"\.llvm\.\d+", "", votes.most_common(1)[0][0].replace("(anonymous namespace)::", ""))
            name = next((short for start, short in WINDOWS_NAMES if name.startswith(start)), re.sub(r"\(.*$", "", name))
        result[tid] = (name or "(other threads)", spin / busy if busy else 0.0)
    return result


def measure(samples, exe, seconds):
    """{thread: (CPU ms a second, of it not spinning)}"""
    prof = stacks.Profile(samples)
    info = analyse(prof, exe)
    threads = collections.defaultdict(lambda: [0.0, 0.0])
    for tid, ms in prof.cpu.items():
        name, spin = info.get(tid, ("(other threads)", 0.0))
        threads[name][0] += ms / seconds
        threads[name][1] += ms * (1.0 - spin) / seconds
    return {k: tuple(v) for k, v in threads.items()}


def one_run(args, exe, keep):
    samples = os.path.join(keep, "samples.txt")
    if not args.again:
        cmd = [sys.executable, PROFILE, "--exe", exe, "--disc", args.disc, "--map", str(args.map), "--seconds",
               str(args.seconds), "--hz", str(args.hz), "--interval-us", str(args.interval_us), "--keep", keep,
               "--top", "1"]
        if args.extra:
            cmd += ["--"] + args.extra
        out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout
        with open(os.path.join(keep, "profile.txt"), "w") as f:
            f.write(out)
        if not os.path.exists(samples):
            sys.stdout.write(out[-3000:])
            sys.exit("build_cost: no samples from %s" % exe)
    return measure(samples, exe, args.seconds)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", action="append", required=True, help="a build's program (two or more)")
    parser.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    parser.add_argument("--map", type=int, default=9)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=15)
    parser.add_argument("--hz", type=int, default=240)
    parser.add_argument("--interval-us", type=int, default=5000)
    parser.add_argument("--threads", type=int, default=8, help="the busiest threads shown")
    parser.add_argument("--keep", help="keep the runs here")
    parser.add_argument("--again", action="store_true", help="no runs: report the runs kept in --keep again")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_args()
    if not args.disc and not args.again:
        sys.exit("build_cost: --disc or KF4_DISC")
    if args.again and not args.keep:
        sys.exit("build_cost: --again wants --keep")
    exes = [os.path.abspath(e) for e in args.exe]
    keep = args.keep or tempfile.mkdtemp(prefix="kf4_build_cost_")
    results = [[] for _ in exes]
    for run in range(args.runs):
        order = [(run + k) % len(exes) for k in range(len(exes))]
        for i in order:
            folder = os.path.join(keep, "run%d_build%d" % (run, i))
            os.makedirs(folder, exist_ok=True)
            r = one_run(args, exes[i], folder)
            results[i].append(r)
            top = sorted(r.items(), key=lambda kv: -kv[1][0])[:5]
            print("run %d build %d: %s" % (run, i, ", ".join("%s %.0f/%.0f" % (k, v[0], v[1]) for k, v in top)),
                  flush=True)
    totals = collections.Counter()
    for runs in results:
        for r in runs:
            for name, v in r.items():
                totals[name] += v[0]
    names = [n for n, _ in totals.most_common(args.threads)]
    print()
    for i, e in enumerate(exes):
        print("build %d: %s" % (i, e))
    print("CPU ms a second of play, then of it not spinning (the thread's work); mean of %d runs +- their standard"
          " deviation. / 60 for ms a vblank" % args.runs)
    print("%-14s" % "thread" + "".join("%34s" % ("build %d" % i) for i in range(len(exes))))
    for name in names:
        row = "%-14s" % name[:14]
        for runs in results:
            cells = []
            for k in (0, 1):
                vals = [r.get(name, (0.0, 0.0))[k] for r in runs]
                sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
                cells.append("%.0f +-%.0f" % (statistics.mean(vals), sd))
            row += "%34s" % " / ".join(cells)
        print(row)
    print("build_cost: runs kept in %s" % keep)


if __name__ == "__main__":
    main()
