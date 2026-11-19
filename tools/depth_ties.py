#!/usr/bin/env python3
"""The depth rule's ties and the world's overdraw, from the PC renderer's developers' counters
(docs/plans/depth-by-layers.md).

    tools/depth_ties.py TOUR_OR_MOTION_DIR [--out REPORT.md] [--json FILE] [--top N]
    tools/depth_ties.py --scenes DIR      (DIR/MODE/SCENE/overdraw.txt and, for the rule, ties.txt with
                                           KF4_PC_TIES_ORDER=1: the world's overdraw and draw order by mode)

The counters (pcr_canvas.cpp's Counters, pcr_prim.h's pcr_count_ties) are written by a run with KF4_PC_TIES and
KF4_PC_OVERDRAW set: `map_tour.py --ties` (NNN/ties.txt, NNN/overdraw.txt, at each view's picture) or
`motion_compare.py --ties` (runs/CASE_PROFILE/ties.txt, at each compared vblank). This reads them all and says:
- for each map or case: the fragments the rule tested and passed at the pictures, and its ties: passed at the same
  depth exactly ("equal": the same plane drawn again; the GPU's own test ties them too, from the same corners), passed
  from behind or ahead within the tolerance, lost (GREATER or past the corners' bound); and the near losses and wins
  (within 2 Z of the tie: what a bias of 2 Z, or the GPU's interpolation of rounded corners, may turn);
- the same by class of draw: its program, its kind (the RdDraw's: map record, object, entity ...), blended or not, a
  decal (pcr_canvas.cpp's is_decal) or not, its Z test and write, its alpha test;
- the draws with the most ties that are not "equal" (at risk with the GPU's depth);
- the world's overdraw: each native batch's fragment shader invocations (a pipeline statistics query) against the
  frame's pixels, by program, at each picture; and from the ties' passed fragments (KF4_PC_TIES_ORDER runs: every slot)
  the draw order's Z.
"""
import argparse
import collections
import glob
import json
import os
import re
import statistics

PROGRAMS = {0: "GIF", 1: "000", 2: "280", 3: "700", 4: "ripple"}
KINDS = {0: "-", 1: "map record", 2: "map object", 3: "item", 4: "gold", 5: "entity", 6: "arm", 7: "effect",
         8: "shadow", 9: "picked", 10: "water"}
ZTST = {0: "NEVER", 1: "ALWAYS", 2: "GEQUAL", 3: "GREATER"}
COUNTS = ("tested", "passed", "equal", "behind", "ahead", "lost", "near_lost", "near_won")


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def parse_t(parts):
    """A T line: the slot's draw and counts"""
    t = {"vblank": int(parts[1]), "slot": int(parts[2]), "draw": int(parts[3]), "v": int(parts[4]),
         "kind": int(parts[5]), "owner": parts[6], "part": int(parts[7]), "record": parts[8],
         "decal": parts[9] == "1", "covers": parts[10] == "1", "layer": parts[11] == "1",
         "sprites": parts[12] == "1", "passes": int(parts[13]), "test": int(parts[14], 16),
         "alpha": int(parts[15], 16), "prim": int(parts[16], 16), "tex0": int(parts[17], 16),
         "zbuf": int(parts[18], 16), "fbp": int(parts[19]), "triangles": int(parts[20])}
    for i, k in enumerate(COUNTS):
        t[k] = int(parts[21 + i])
    t["zmin"], t["zmax"] = float(parts[29]), float(parts[30])
    return t


def draw_class(t):
    """What kind of draw a slot is, as the data names it before drawing"""
    test, prim, zbuf = t["test"], t["prim"], t["zbuf"]
    blended = bits(prim, 6, 1) == 1
    ate = bits(test, 0, 1) == 1 and bits(test, 1, 3) != 1
    zwrite = bits(zbuf, 32, 1) == 0
    parts = [PROGRAMS.get(t["v"], str(t["v"])), KINDS.get(t["kind"], str(t["kind"]))]
    if t["v"] == 0:
        parts.append("sprites" if t["sprites"] else "triangles")
    parts.append("decal" if t["decal"] else ("blended, covers" if blended else "opaque"))
    parts.append(ZTST[bits(test, 17, 2)] + (" writes Z" if zwrite else " no Z write"))
    if ate:
        parts.append("alpha tested")
    if bits(test, 14, 1):
        parts.append("DATE")
    return " | ".join(parts)


def read_report_views(folder):
    """map_tour's views: NNN -> [(view, picture_at)]"""
    path = os.path.join(folder, "report.json")
    if not os.path.isfile(path):
        return {}
    with open(path) as f:
        rep = json.load(f)
    out = {}
    if not isinstance(rep, dict):
        return out  # (motion_compare's report: its counters are at its compared vblanks already)
    for m in rep.get("maps", []):
        out["%03d" % m["map"]] = [(i, v.get("picture_at")) for i, v in enumerate(m.get("views", []))]
    return out


def sources(folder):
    """(name, ties.txt, overdraw.txt) of a tour (NNN/) or a motion comparison (runs/CASE_PROFILE/)"""
    found = []
    for t in sorted(glob.glob(os.path.join(folder, "*", "ties.txt"))):
        found.append((os.path.basename(os.path.dirname(t)), t))
    for t in sorted(glob.glob(os.path.join(folder, "runs", "*", "ties.txt"))):
        found.append((os.path.basename(os.path.dirname(t)), t))
    return [(n, t, os.path.join(os.path.dirname(t), "overdraw.txt")) for n, t in found]


def read_ties(path, keep_vblanks=None):
    totals, slots = {}, []
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split()
            if not p or p[0] == "#":
                continue
            if p[0] == "V":
                v = int(p[1])
                if keep_vblanks is not None and v not in keep_vblanks:
                    continue
                totals[v] = dict(zip(("slots",) + COUNTS, map(int, p[2:12])))
            elif p[0] == "T":
                t = parse_t(p)
                if keep_vblanks is not None and t["vblank"] not in keep_vblanks:
                    continue
                slots.append(t)
    return totals, slots


def read_overdraw(path, keep_vblanks=None):
    """vblank -> list of (v, fbp, pixels, date, from_surface, decal, blended, batches, invocations)"""
    out = collections.defaultdict(list)
    if not os.path.isfile(path):
        return out
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p or p[0] != "Q":
                continue
            v = int(p[1])
            if keep_vblanks is not None and v not in keep_vblanks:
                continue
            out[v].append((int(p[2]), int(p[3], 16), int(p[4]), p[5] == "1", p[6] == "1", p[7] == "1", p[8] == "1",
                           int(p[9]), int(p[10])))
    return out


def world_overdraw(rows):
    """A picture's world (native programs, not DATE, not read from a surface: the sky) into its main frame buffer: its
    fragment shader invocations by program, and that buffer's pixels"""
    by_fbp = collections.Counter()
    for v, fbp, pixels, date, surf, decal, blended, batches, inv in rows:
        if not date and not surf:
            by_fbp[(fbp, pixels)] += inv
    if not by_fbp:
        return None
    (fbp, pixels), _ = by_fbp.most_common(1)[0]
    prog = collections.Counter()
    blended_inv = 0
    for v, f, px, date, surf, decal, blended, batches, inv in rows:
        if f == fbp and not date and not surf:
            prog[PROGRAMS.get(v, str(v))] += inv
            blended_inv += inv if blended else 0
    total = sum(prog.values())
    return {"fbp": fbp, "pixels": pixels, "invocations": total, "per_pixel": total / pixels if pixels else 0,
            "by_program": {k: c / pixels for k, c in prog.items()}, "blended_per_pixel": blended_inv / pixels}


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def read_w(path):
    """vblank -> program -> counts (the W lines: every slot of the program, summed)"""
    out = collections.defaultdict(dict)
    if not os.path.isfile(path):
        return out
    with open(path) as f:
        for line in f:
            p = line.split()
            if p and p[0] == "W":
                out[int(p[1])][int(p[2])] = dict(zip(COUNTS, map(int, p[3:11])))
    return out


def spearman(xs, ys):
    def ranks(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        for k, i in enumerate(order):
            r[i] = float(k)
        return r
    if len(xs) < 3:
        return 0.0
    rx, ry = ranks(xs), ranks(ys)
    mx, my = statistics.mean(rx), statistics.mean(ry)
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    den = (sum((a - mx) ** 2 for a in rx) * sum((b - my) ** 2 for b in ry)) ** 0.5
    return num / den if den else 0.0


def scenes_report(folder):
    """DIR/MODE/SCENE: the world's fragment shader invocations a pixel by mode, and (the rule's KF4_PC_TIES_ORDER) its
    fragments tested and passed a pixel and the order of its opaque batches' Z"""
    lines = ["| scene | mode | pictures | invocations a pixel (000, 280, 700) | tested a pixel | passed a pixel | "
             "opaque passed a pixel | order: Z against draw order (opaque: not decals) |", "|---|---|---|---|---|---|---|---|"]
    for mode in sorted(m for m in os.listdir(folder) if os.path.isdir(os.path.join(folder, m))):
        for scene in sorted(os.listdir(os.path.join(folder, mode))):
            d = os.path.join(folder, mode, scene)
            od = read_overdraw(os.path.join(d, "overdraw.txt"))
            pics = []
            for v, rows in sorted(od.items()):
                w = world_overdraw(rows) if v >= 700 else None
                if w:
                    w["vblank"] = v
                    pics.append(w)
            fbp_at = {p["vblank"]: p["fbp"] for p in pics}
            if not pics:
                continue
            inv = statistics.median(p["per_pixel"] for p in pics)
            progs = {k: statistics.median(p["by_program"].get(k, 0) for p in pics) for k in ("000", "280", "700")}
            pixels = pics[0]["pixels"]
            tested = passed = opaque = order = "-"
            tpath = os.path.join(d, "ties.txt")
            if os.path.isfile(tpath):
                w = read_w(tpath)
                vb = [v for v in w if v in fbp_at]
                tested = "%.2f" % statistics.median(sum(w[v].get(k, {}).get("tested", 0) for k in (1, 2, 3)) / pixels
                                                    for v in vb)
                passed = "%.2f" % statistics.median(sum(w[v].get(k, {}).get("passed", 0) for k in (1, 2, 3)) / pixels
                                                    for v in vb)
                _, slots = read_ties(tpath)
                ops, cors = [], []
                for v in sorted({t["vblank"] for t in slots} & set(fbp_at)):
                    ss = [t for t in slots if t["vblank"] == v and t["v"] in (1, 2, 3) and not t["decal"]
                          and t["fbp"] == fbp_at[v] and t["tested"] > 0]
                    ops.append(sum(t["passed"] for t in ss) / pixels)
                    cors.append(spearman([t["slot"] for t in ss], [(t["zmin"] + t["zmax"]) / 2 for t in ss]))
                opaque = "%.2f" % statistics.median(ops)
                order = "%+.2f" % statistics.median(cors)
            lines.append("| %s | %s | %d | %.2f (%.2f, %.2f, %.2f) | %s | %s | %s | %s |" % (
                scene, mode, len(pics), inv, progs["000"], progs["280"], progs["700"], tested, passed, opaque, order))
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--scenes", action="store_true", help="FOLDER/MODE/SCENE: the overdraw and draw order by mode")
    ap.add_argument("--out", help="report.md (default: FOLDER/depth_ties.md)")
    ap.add_argument("--json", help="the numbers (default: FOLDER/depth_ties.json)")
    ap.add_argument("--top", type=int, default=40)
    a = ap.parse_args()
    if a.scenes:
        text = scenes_report(a.folder)
        with open(a.out or os.path.join(a.folder, "overdraw.md"), "w") as f:
            f.write(text + "\n")
        print(text)
        return
    views = read_report_views(a.folder)
    srcs = sources(a.folder)
    if not srcs:
        raise SystemExit("depth_ties: no ties.txt under %s" % a.folder)
    per_src = []
    classes = collections.defaultdict(lambda: collections.Counter())
    class_where = collections.defaultdict(set)
    risky = collections.defaultdict(lambda: collections.Counter())
    risky_info = {}
    overdraw = []
    for name, tpath, opath in srcs:
        keep = None
        if name in views:
            keep = {p for _, p in views[name] if p is not None}
        totals, slots = read_ties(tpath, keep)
        if keep is None and slots:
            # (a motion case: its compared vblanks, those its details were written at)
            at = {t["vblank"] for t in slots}
            totals = {v: t for v, t in totals.items() if v in at}
        od = read_overdraw(opath, keep)
        s = collections.Counter()
        for v, t in totals.items():
            for k in COUNTS:
                s[k] += t[k]
        s["pictures"] = len(totals)
        cls = collections.Counter()
        for t in slots:
            c = draw_class(t)
            for k in COUNTS:
                classes[c][k] += t[k]
            classes[c]["slots"] += 1
            classes[c]["slots_tying"] += 1 if t["behind"] + t["ahead"] + t["lost"] else 0
            classes[c]["slots_equal"] += 1 if t["equal"] else 0
            class_where[c].add(name)
            at_risk = t["behind"] + t["ahead"] + t["lost"]
            if at_risk or t["equal"]:
                key = (name, t["v"], t["kind"], t["owner"], t["part"], c)
                for k in COUNTS:
                    risky[key][k] += t[k]
                risky[key]["pictures"] += 1
                risky_info[key] = t
            cls[c] += at_risk
        pics = []
        for v, rows in sorted(od.items()):
            w = world_overdraw(rows)
            if w:
                w["vblank"] = v
                pics.append(w)
        if pics:
            overdraw.append({"name": name, "pictures": len(pics),
                             "per_pixel_median": statistics.median(p["per_pixel"] for p in pics),
                             "per_pixel_max": max(p["per_pixel"] for p in pics),
                             "blended_per_pixel_median": statistics.median(p["blended_per_pixel"] for p in pics),
                             "by_program_median": {k: statistics.median(p["by_program"].get(k, 0) for p in pics)
                                                   for k in ("000", "280", "700", "ripple")}})
        per_src.append({"name": name, **s, "top_class": cls.most_common(1)[0][0] if cls and cls.most_common(1)[0][1] else ""})

    out = a.out or os.path.join(a.folder, "depth_ties.md")
    lines = ["# The depth rule's ties and the world's overdraw: %s" % a.folder, "",
             "From `tools/depth_ties.py` (the PC renderer's developers' counters, KF4_PC_TIES and KF4_PC_OVERDRAW). "
             "Fragments at the pictures counted (a tour's views; a motion case's compared vblanks). Equal: a tie at the "
             "same depth exactly; behind/ahead: a tie passed within the tolerance; lost: a tie failed (GREATER, the "
             "corners' bound); near: within 2 Z past the tolerance.", ""]
    all_s = collections.Counter()
    for s in per_src:
        for k in COUNTS + ("pictures",):
            all_s[k] += s.get(k, 0)
    lines += ["## All", "",
              "| pictures | tested | passed | equal ties | behind | ahead | lost | near lost | near won |",
              "|---|---|---|---|---|---|---|---|---|",
              "| %d | %d | %d | %d (%.2f%%) | %d (%.4f%%) | %d (%.4f%%) | %d | %d | %d |" % (
                  all_s["pictures"], all_s["tested"], all_s["passed"], all_s["equal"], pct(all_s["equal"], all_s["passed"]),
                  all_s["behind"], pct(all_s["behind"], all_s["passed"]), all_s["ahead"],
                  pct(all_s["ahead"], all_s["passed"]), all_s["lost"], all_s["near_lost"], all_s["near_won"]), ""]
    lines += ["## By map or case", "",
              "Ties not equal: behind + ahead + lost, the fragments whose winner the GPU's own depth may pick otherwise "
              "(a share of the fragments passed).", "",
              "| map/case | pictures | passed | equal | not equal | of passed | near lost | near won | most of them |",
              "|---|---|---|---|---|---|---|---|---|"]
    for s in sorted(per_src, key=lambda s: -(s["behind"] + s["ahead"] + s["lost"])):
        ne = s["behind"] + s["ahead"] + s["lost"]
        lines.append("| %s | %d | %d | %d | %d | %.4f%% | %d | %d | %s |" % (
            s["name"], s["pictures"], s["passed"], s["equal"], ne, pct(ne, s["passed"]), s["near_lost"],
            s["near_won"], s["top_class"]))
    lines += ["", "## By class of draw", "",
              "Every slot with a tie or a near one (the counters write only those; with KF4_PC_TIES_ORDER every slot), "
              "summed: what kind of draw ties. Slots: draws (a native batch's run: one draw's), counted at each picture; "
              "tying: with ties not equal; equal: with equal ties.", "",
              "| class | slots | tying | equal | tested | equal ties | behind | ahead | lost | near lost | near won | maps/cases |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for c, k in sorted(classes.items(), key=lambda kv: -(kv[1]["behind"] + kv[1]["ahead"] + kv[1]["lost"] + kv[1]["equal"])):
        lines.append("| %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d | %d |" % (
            c, k["slots"], k["slots_tying"], k["slots_equal"], k["tested"], k["equal"], k["behind"], k["ahead"], k["lost"], k["near_lost"], k["near_won"],
            len(class_where[c])))
    lines += ["", "## The draws with the most ties not equal", "",
              "| map/case | program | kind | owner | part | class | pictures | equal | behind | ahead | lost | near lost |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    top = sorted(risky.items(), key=lambda kv: -(kv[1]["behind"] + kv[1]["ahead"] + kv[1]["lost"]))[:a.top]
    for (name, v, kind, owner, part, c), k in top:
        lines.append("| %s | %s | %s | %s | %d | %s | %d | %d | %d | %d | %d | %d |" % (
            name, PROGRAMS.get(v, v), KINDS.get(kind, kind), owner, part, c, k["pictures"], k["equal"], k["behind"],
            k["ahead"], k["lost"], k["near_lost"]))
    if overdraw:
        lines += ["", "## The world's overdraw", "",
                  "Fragment shader invocations of the native batches (not DATE's, not the sky's) into the picture's frame "
                  "buffer, a pixel of it, the median of the pictures (the depth rule's shaders: the GPU's early test "
                  "with the corners' bound still rejects some).", "",
                  "| map/case | pictures | world | of it blended | 000 | 280 | 700 | most |", "|---|---|---|---|---|---|---|---|"]
        for o in sorted(overdraw, key=lambda o: -o["per_pixel_median"]):
            b = o["by_program_median"]
            lines.append("| %s | %d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |" % (
                o["name"], o["pictures"], o["per_pixel_median"], o["blended_per_pixel_median"], b["000"], b["280"],
                b["700"], o["per_pixel_max"]))
        med = statistics.median(o["per_pixel_median"] for o in overdraw)
        lines += ["", "Median over maps/cases: %.2f fragments a pixel." % med]
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(a.json or os.path.join(a.folder, "depth_ties.json"), "w") as f:
        json.dump({"sources": per_src, "classes": {c: dict(k, maps=sorted(class_where[c])) for c, k in classes.items()},
                   "overdraw": overdraw,
                   "top": [{"where": name, "program": PROGRAMS.get(v, v), "kind": KINDS.get(kind, kind), "owner": owner,
                            "part": part, "class": c, **k} for (name, v, kind, owner, part, c), k in top]},
                  f, indent=1)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()
