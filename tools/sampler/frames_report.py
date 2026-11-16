#!/usr/bin/env python3
"""What a frame profiler CSV says (KF4_PROFILE_OUT, docs/port/PROFILING.md "Frames"): the pictures' lengths, each
lane's busiest zones in ms a picture, and the slowest pictures with what they spent their time on.

    python3 tools/sampler/frames_report.py FILE [--skip N] [--top 15] [--lane NAME] [--slowest 5] [--compare FILE2]

--skip drops the first N pictures (start-up, the first pipelines made). --compare prints FILE2's figures beside
FILE's, zone by zone (an A/B of two builds or settings).
"""
import argparse
import csv
import statistics
import sys
from collections import defaultdict


def load(path, skip):
    """The pictures: index -> (length ms, {(lane, zone, kind): (self ms, incl ms, calls)})"""
    frames = {}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            index = int(row['frame'])
            if row['zone'] == 'frame' and row['lane'] == '':
                frames.setdefault(index, [0.0, {}])[0] = float(row['self_ms'])
                continue
            entry = frames.setdefault(index, [0.0, {}])
            key = (row['lane'], row['zone'], row['kind'])
            entry[1][key] = (float(row['self_ms']), float(row['incl_ms']), int(row['calls'] or 0))
    ordered = sorted(frames)[skip:]
    return {i: frames[i] for i in ordered}


def per_picture(frames):
    """(lane, zone, kind) -> [self ms a picture, incl ms a picture, calls a picture]"""
    sums = defaultdict(lambda: [0.0, 0.0, 0.0])
    for _, zones in frames.values():
        for key, (self_ms, incl_ms, calls) in zones.items():
            s = sums[key]
            s[0] += self_ms
            s[1] += incl_ms
            s[2] += calls
    n = max(len(frames), 1)
    return {k: [v / n for v in s] for k, s in sums.items()}


def lengths_line(frames):
    lengths = sorted(f[0] for f in frames.values())
    if not lengths:
        return 'no pictures'
    p99 = lengths[min(len(lengths) - 1, int(len(lengths) * 0.99))]
    return (f'{len(lengths)} pictures: {statistics.mean(lengths):.3f} ms average, median '
            f'{statistics.median(lengths):.3f}, p99 {p99:.3f}, most {lengths[-1]:.3f}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('file')
    ap.add_argument('--skip', type=int, default=0)
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--lane')
    ap.add_argument('--slowest', type=int, default=5)
    ap.add_argument('--compare')
    args = ap.parse_args()

    a = load(args.file, args.skip)
    b = load(args.compare, args.skip) if args.compare else None
    print(f'{args.file}: {lengths_line(a)}')
    if b is not None:
        print(f'{args.compare}: {lengths_line(b)}')
    pa = per_picture(a)
    pb = per_picture(b) if b is not None else {}

    lanes = sorted({k[0] for k in pa} | {k[0] for k in pb})
    for lane in lanes:
        if args.lane and lane != args.lane:
            continue
        keys = [k for k in set(pa) | set(pb) if k[0] == lane]
        keys.sort(key=lambda k: -max(pa.get(k, [0])[0], pb.get(k, [0])[0]))
        busy = sum(v[0] for k, v in pa.items() if k[0] == lane and k[2] != 'wait')
        print(f'\n{lane}: {busy:.3f} ms a picture busy (self, waits left out)')
        print(f'  {"self ms":>9} {"incl ms":>9} {"calls":>8}' + (f' {"B self":>9} {"B-A":>8}' if b else '') + '  zone')
        for k in keys[:args.top]:
            s, i, c = pa.get(k, [0, 0, 0])
            line = f'  {s:9.4f} {i:9.4f} {c:8.1f}'
            if b is not None:
                sb = pb.get(k, [0, 0, 0])[0]
                line += f' {sb:9.4f} {sb - s:+8.4f}'
            print(f'{line}  {k[1]}{" (wait)" if k[2] == "wait" else ""}')

    if args.slowest:
        print(f'\nthe {args.slowest} slowest pictures:')
        for index, (ms, zones) in sorted(a.items(), key=lambda kv: -kv[1][0])[:args.slowest]:
            top = sorted(((v[0], k) for k, v in zones.items() if k[2] != 'wait'), reverse=True)[:6]
            parts = ', '.join(f'{k[0]}/{k[1]} {s:.2f}' for s, k in top)
            print(f'  picture {index}: {ms:.2f} ms: {parts}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
