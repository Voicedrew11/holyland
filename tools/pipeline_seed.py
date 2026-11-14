#!/usr/bin/env python3
"""The PC renderer's pipeline seed (port/gs/pc/pipeline_seed.bin): every pipeline the canvas draws with in a tour of
the game, made before play on a first start (port/gs/pc/pcr_pipelines.cpp, "Made before play").

    tools/pipeline_seed.py --work DIR [--exe EXE] [--disc ISO] [--profiles 1x,msaa,ps2] [--jobs 2]
                           [--maps 0,99,...] [--no-tour] [--no-cases] [--merge-only] [--out FILE]

For each profile (the settings that change which pipelines there are: MSAA and the cut-outs shaded a sample at a
time, the native draws on or off; the scale, anisotropy, the draw distance and the like do not), with
KF4_PC_DYNAMIC=0 (no dynamic state, so that every state a draw has makes a pipeline of its own: a device with
dynamic state makes those that differ only there one), KF4_INTERP_DRAW_ALL=1 and --frame-rate 240 (the pictures
between and their runs):
1. tools/map_tour.py over every map (--keep-lists): each map's views, from a data folder of its own;
2. tools/motion_compare.py's cases (the title, every menu page, an item picked up, combat, spells, creatures'
   effects, under water, the drain), each from a data folder of its own;
each run's pc_pipeline_list.bin kept in DIR/<profile>/. Then every list is merged into the seed: each record with
the MSAA samples of its run (1, or the profile's), the same ones once, in a fixed order.

Colour high is only run through the cases (profiles high, highmsaa): it changes the colour surfaces' format, which
the renderer puts in, and adds its own full-screen passes (the scanout narrowed to 8 bits), drawn the same anywhere. The records
are the renderer's state only (programs by the canvas's index, Granite's pipeline state, attachment formats):
none of the game's data, so the seed may be in the repository. A record's layout is Granite's: when Granite's
pipeline state changes, the seed's header no longer matches and it is not used ("[pcr] the pipeline seed is not of
this build's layout"); make it again. Runs are headless, --no-audio; DIR (pictures, saves) outside the repository.
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SEED = os.path.join(ROOT, "port", "gs", "pc", "pipeline_seed.bin")

# name -> (the MSAA samples its passes draw at, the run's options)
PROFILES = {
    "1x": (1, []),
    "msaa": (4, ["--native-msaa", "4", "--native-cutout-aa", "ssaa"]),
    "ps2": (1, ["--native-draw", "off"]),
    "ps2msaa": (4, ["--native-draw", "off", "--native-msaa", "4", "--native-cutout-aa", "ssaa"]),
    # colour high: only its own passes (the scanout narrowed to 8 bits, the expand at 16): the cases suffice
    "high": (1, ["--native-colour", "high"]),
    "highmsaa": (4, ["--native-colour", "high", "--native-msaa", "4", "--native-cutout-aa", "ssaa"]),
}
NO_TOUR = {"high", "highmsaa"}
COMMON = ["--frame-rate", "240"]

LIST_MAGIC = b"KF4PCPL1"
LIST_HEADER = struct.Struct("<8s8I")   # magic, record, state, potential, subpass sizes, attribs, buffers, count, -
SEED_MAGIC = b"KF4PCPS1"


def env_for(data):
    env = dict(os.environ, KF4_DATA_DIR=data, KF4_PC_DYNAMIC="0", KF4_PC_PRECOMPILE="0", KF4_INTERP_DRAW_ALL="1")
    env.pop("KF4_FX", None)
    return env


def tour(a, name, opts):
    keep = os.path.join(a.work, name, "maps")
    cmd = [sys.executable, os.path.join(HERE, "map_tour.py"), "--out", os.path.join(a.work, name, "tour"),
           "--exe", a.exe, "--disc", a.disc, "--jobs", str(a.jobs), "--no-capture", "--keep-lists", keep,
           "--extra", " ".join(opts + COMMON)]
    if a.maps:
        cmd += ["--maps", a.maps]
    env = env_for(os.path.join(a.work, name, "unused"))
    print("[seed] %s: the map tour" % name, flush=True)
    subprocess.call(cmd, env=env, stdout=subprocess.DEVNULL)


def case_run(a, name, opts, case_name, case):
    import motion_compare  # (its cases and pads)
    d = os.path.join(a.work, name, "cases", motion_compare.slug(case_name))
    shutil.rmtree(d, ignore_errors=True)
    data = os.path.join(d, "data")
    os.makedirs(data)
    pad = os.path.join(d, "input.pad")
    motion_compare.write_pad(pad, case)
    last = max(case["frames"]) + 2
    cmd = [a.exe, "--disc", a.disc, "--headless", "--no-audio", "--fast", "--memcard", "none", "--input", pad,
           "--frames", str(last), "--dump-frames", str(last - 1), "--dump-dir", d] + case["args"] + opts + COMMON
    env = env_for(data)
    if case["fx"]:
        env["KF4_FX"] = case["fx"]
    with open(os.path.join(d, "out.txt"), "w") as f:
        rc = subprocess.call(cmd, env=env, stdout=f, stderr=subprocess.STDOUT)
    made = os.path.join(data, "pc_pipeline_list.bin")
    if os.path.isfile(made):
        os.makedirs(os.path.join(a.work, name, "lists"), exist_ok=True)
        shutil.copyfile(made, os.path.join(a.work, name, "lists", motion_compare.slug(case_name) + ".bin"))
    shutil.rmtree(d, ignore_errors=True)
    return case_name, rc


def cases(a, name, opts):
    sys.path.insert(0, HERE)
    import motion_compare
    print("[seed] %s: %d cases" % (name, len(motion_compare.CASES)), flush=True)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for case_name, rc in pool.map(lambda kv: case_run(a, name, opts, kv[0], kv[1]), motion_compare.CASES.items()):
            if rc != 0:
                print("[seed] %s: case %s exit %d" % (name, case_name, rc), flush=True)


def read_list(path):
    b = open(path, "rb").read()
    if len(b) < LIST_HEADER.size:
        return None, []
    h = LIST_HEADER.unpack_from(b)
    if h[0] != LIST_MAGIC:
        return None, []
    size, count = h[1], h[7]
    if len(b) != LIST_HEADER.size + size * count:
        return None, []
    return h, [b[LIST_HEADER.size + i * size:LIST_HEADER.size + (i + 1) * size] for i in range(count)]


def merge(a):
    layout, entries = None, set()
    lists = 0
    for name in a.profiles:
        samples = PROFILES[name][0]
        for sub in ("maps", "lists"):
            folder = os.path.join(a.work, name, sub)
            if not os.path.isdir(folder):
                continue
            for f in sorted(os.listdir(folder)):
                h, records = read_list(os.path.join(folder, f))
                if h is None:
                    print("[seed] %s: not a pipeline list" % os.path.join(folder, f))
                    continue
                if layout is None:
                    layout = h[1:7]
                elif h[1:7] != layout:
                    sys.exit("pipeline_seed: %s is of another layout (another build?)" % os.path.join(folder, f))
                lists += 1
                for r in records:
                    entries.add((samples, r))
    if layout is None:
        sys.exit("pipeline_seed: no pipeline lists under %s" % a.work)
    record_size = layout[0]
    entry_size = 8 + record_size
    ordered = sorted(entries)
    with open(a.out, "wb") as f:
        # (pcr_pipelines.cpp's SeedHeader: magic, the entry's size, then the list's layout, the count)
        f.write(struct.pack("<8s8I", SEED_MAGIC, entry_size, layout[1], layout[2], layout[3], layout[4], layout[5],
                            len(ordered), 0))
        for samples, r in ordered:
            f.write(struct.pack("<II", samples, 0) + r)
    by = {}
    for samples, _ in ordered:
        by[samples] = by.get(samples, 0) + 1
    print("[seed] %s: %d records (%s) from %d lists" % (a.out, len(ordered),
          ", ".join("%d at %dx" % (n, s) for s, n in sorted(by.items())), lists))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--work", required=True, help="a folder outside the repository for the runs")
    p.add_argument("--exe", default=os.path.join(ROOT, "out", "windows-clang" if os.name == "nt" else "linux-clang",
                                                 "kf4.exe" if os.name == "nt" else "kf4"))
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--profiles", default=",".join(PROFILES))
    p.add_argument("--jobs", type=int, default=2)
    p.add_argument("--maps", help="map numbers for the tour (default: all)")
    p.add_argument("--no-tour", action="store_true")
    p.add_argument("--no-cases", action="store_true")
    p.add_argument("--merge-only", action="store_true", help="only merge the lists already in --work")
    p.add_argument("--out", default=SEED)
    a = p.parse_args()
    a.profiles = a.profiles.split(",")
    a.work = os.path.abspath(a.work)
    if a.work == ROOT or a.work.startswith(ROOT + os.sep):
        sys.exit("pipeline_seed: --work must be outside the repository")
    if not a.merge_only:
        if not a.disc:
            sys.exit("pipeline_seed: pass --disc or set KF4_DISC")
        for name in a.profiles:
            opts = PROFILES[name][1]
            if not a.no_tour and name not in NO_TOUR:
                tour(a, name, opts)
            if not a.no_cases:
                cases(a, name, opts)
    merge(a)


if __name__ == "__main__":
    main()
