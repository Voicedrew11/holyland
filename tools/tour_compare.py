#!/usr/bin/env python3
"""Two map tours compared picture by picture: the PC renderer's against paraLLEl-GS's.

    tools/tour_compare.py PC_TOUR REF_TOUR --out DIR [--top N] [--level L]

PC_TOUR and REF_TOUR are map_tour.py output folders of the same maps, places and headings, e.g.

    tools/map_tour.py --out ~/.cache/tour/pc --renderer pc --extra "--native-scale 1 --no-native-widescreen"
    tools/map_tour.py --out ~/.cache/tour/gs --renderer gs

Each view's two pictures are paired (the larger brought down to the smaller's size) and measured:
- mean: the mean difference a pixel (the largest of its three channels, 0-255);
- over: the share of pixels whose difference is over --level (default 24);
- blurred: the same share after both pictures are blurred 3 x 3, which leaves out most of what filtering
  and edges change and keeps what is drawn differently (missing, extra, wrong colour or place). Views
  are ranked by it.
A view is "not comparable" when the two runs' game state at the picture differs (map, position), or a
picture is missing.

Into --out: report.md (the ranking), report.html (the --top views as PC | reference | difference),
report.json (every view), and pairs/NNN_vXX.png for the --top views. The pictures hold the game's data:
keep --out outside the repository.
"""
import argparse
import html
import json
import os
import sys

import numpy as np
from PIL import Image, ImageFilter


def load_tour(folder):
    with open(os.path.join(folder, "report.json")) as f:
        return {m["map"]: m for m in json.load(f)["maps"]}


def same_state(a, b):
    sa, sb = a.get("state"), b.get("state")
    if not sa or not sb:
        return False
    if sa["map"] != sb["map"]:
        return False
    return all(abs(x - y) < 0.5 for x, y in zip(sa["pos"], sb["pos"]))


def pictures_at_one_size(path_a, path_b):
    a = Image.open(path_a).convert("RGB")
    b = Image.open(path_b).convert("RGB")
    if a.size == b.size:
        return a, b
    if a.size[0] * a.size[1] > b.size[0] * b.size[1]:
        return a.resize(b.size, Image.BOX), b
    return a, b.resize(a.size, Image.BOX)


def difference(a, b):
    """The per-pixel difference: the largest of the three channels' (0-255)."""
    return np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).max(axis=2)


def measure(a, b, level):
    d = difference(a, b)
    blurred = difference(a.filter(ImageFilter.BoxBlur(1)), b.filter(ImageFilter.BoxBlur(1)))
    return {"mean": round(float(d.mean()), 2), "over": round(float((d > level).mean()), 4),
            "blurred": round(float((blurred > level).mean()), 4), "worst": int(d.max())}, d


def pair_picture(a, b, d, path):
    """PC | reference | the difference (x4, red where over the level)."""
    w, h = a.size
    heat = np.clip(d.astype(np.int32) * 4, 0, 255).astype(np.uint8)
    diff = Image.fromarray(np.stack([heat, heat // 3, heat // 3], axis=2), "RGB")
    out = Image.new("RGB", (w * 3, h))
    out.paste(a, (0, 0))
    out.paste(b, (w, 0))
    out.paste(diff, (w * 2, 0))
    out.save(path)


def compare(args):
    pc, ref = load_tour(args.pc), load_tour(args.ref)
    rows, problems = [], []
    for number in sorted(set(pc) | set(ref)):
        if number not in pc or number not in ref:
            problems.append("map %03d: only in %s" % (number, "PC" if number in pc else "reference"))
            continue
        pv, rv = pc[number]["views"], ref[number]["views"]
        if len(pv) != len(rv):
            problems.append("map %03d: %d views against %d" % (number, len(pv), len(rv)))
        for i, (a, b) in enumerate(zip(pv, rv)):
            row = {"map": number, "name": pc[number].get("name") or "", "view": i, "picture_at": a["picture_at"],
                   "where": a.get("where"), "pc_flags": a["flags"], "ref_flags": b["flags"]}
            if not a.get("picture") or not b.get("picture"):
                row["skip"] = "picture missing (%s)" % ("PC" if not a.get("picture") else "reference")
            elif not same_state(a, b):
                row["skip"] = "game state differs"
            if "skip" in row:
                problems.append("map %03d view %d: %s" % (number, i, row["skip"]))
                rows.append(row)
                continue
            pa, pb = pictures_at_one_size(a["picture"], b["picture"])
            m, _ = measure(pa, pb, args.level)
            row.update(m)
            row["pc_picture"], row["ref_picture"] = a["picture"], b["picture"]
            rows.append(row)
    return rows, problems


def write_reports(rows, problems, args):
    pairs = os.path.join(args.out, "pairs")
    os.makedirs(pairs, exist_ok=True)
    ranked = sorted((r for r in rows if "skip" not in r), key=lambda r: (-r["blurred"], -r["mean"]))
    top = ranked[:args.top]
    for r in top:
        pa, pb = pictures_at_one_size(r["pc_picture"], r["ref_picture"])
        _, d = measure(pa, pb, args.level)
        r["pair"] = "pairs/%03d_v%02d.png" % (r["map"], r["view"])
        pair_picture(pa, pb, d, os.path.join(args.out, r["pair"]))

    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump({"level": args.level, "views": rows, "problems": problems}, f, indent=1)

    def line(r):
        return "| %03d | %s | %d | %.2f%% | %.2f%% | %.2f | %d |" % (
            r["map"], r["name"], r["view"], r["blurred"] * 100, r["over"] * 100, r["mean"], r["worst"])

    md = ["# PC renderer against the reference, by map tour view", "",
          "%d views compared, %d not. Ranked by the share of pixels over %d levels after a 3 x 3 blur "
          "(what is drawn differently, not filtering or edges)." % (len(ranked), len(rows) - len(ranked), args.level),
          "", "| map | name | view | blurred over | over | mean | worst |", "|---|---|---|---|---|---|---|"]
    md += [line(r) for r in ranked]
    if problems:
        md += ["", "## Not compared", ""] + ["- " + p for p in problems]
    with open(os.path.join(args.out, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(md) + "\n")

    cells = "".join(
        "<h3>%03d %s, view %d: blurred %.2f%%, over %.2f%%, mean %.2f</h3><img src='%s' width=1440 loading=lazy>" % (
            r["map"], html.escape(r["name"]), r["view"], r["blurred"] * 100, r["over"] * 100, r["mean"], r["pair"])
        for r in top)
    page = ("<!doctype html><meta charset=utf-8><title>Tour compare</title><style>body{font:13px sans-serif;"
            "background:#111;color:#ddd}</style><h1>PC | reference | difference</h1><p>The %d views that differ "
            "most; every view in report.md.</p>%s" % (len(top), cells))
    with open(os.path.join(args.out, "report.html"), "w", encoding="utf-8") as f:
        f.write(page)
    return ranked


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pc", help="the PC renderer's map tour folder")
    parser.add_argument("ref", help="the reference's (--renderer gs) map tour folder")
    parser.add_argument("--out", required=True, help="the output folder (outside the repository)")
    parser.add_argument("--top", type=int, default=40, help="views pictured in report.html (default 40)")
    parser.add_argument("--level", type=int, default=24, help="a pixel differs over this many levels (default 24)")
    args = parser.parse_args()
    rows, problems = compare(args)
    ranked = write_reports(rows, problems, args)
    for r in ranked[:10]:
        print("map %03d view %2d: blurred %.2f%%, over %.2f%%, mean %.2f" % (
            r["map"], r["view"], r["blurred"] * 100, r["over"] * 100, r["mean"]))
    print("tour_compare: %d views compared, %d not; %s" % (len(ranked), len(rows) - len(ranked),
                                                          os.path.join(args.out, "report.html")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
