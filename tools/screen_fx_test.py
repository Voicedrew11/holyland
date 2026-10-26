#!/usr/bin/env python3
"""Every full-screen and screen-reading effect of the game (docs/port/SCREEN_EFFECTS.md), each made to happen
headless, measured at the owner's settings and checked for broken pictures between.

    python tools/screen_fx_test.py --out DIR [--exe out/windows-clang/kf4.exe] [--iso ISO] [--save QUICKSAVE]
                                   [--spots DIR] [--cases a,b,...] [--perf] [--pictures] [--frames N] [--keep]
                                   [--renderer modern] [-- extra kf4 options]

Each case is a run of the game (always --no-audio, --headless, a fresh data folder in DIR):
- the forced ones load --save (a quick save in play; without it a new game, tests/newgame.pad, and the cases
  start 1,300 vblanks later) and turn their effect on with KF4_FX (game/screen_fx.c: the passes' state set
  for the draw only, so the game itself goes on as without it);
- some go somewhere first (--teleport), or press buttons (the pause, the menu, casting a spell);
- the real scenes need quick saves from --spots: quick_underwater.kf4quick (walking forward into the water),
  quick_deathwater.kf4quick (falling into deadly water), quick_palace.kf4quick (the Queen's Palace, the MP
  drain), quick_wand.kf4quick (standing on a Symbol of Passage: the Item screen, Up x4 to the Green Wand, used:
  the map change's fade of its last frame, 0x2BC0), quick_icy.kf4quick (the icy corridor and its creatures: their
  shine drawn whole, and with --native-shine exact), quick_succubus_hotspot.kf4quick (map 038 at the Dark
  Succubus' room: her body drawn with a copy of the screen; and up close, KF4_FX=succubus). Cases whose save is
  missing are left out.

--perf (real time, KF4_PRESENT_SIM=240, KF4_GSN_GPU_TIME=2, KF4_HITCH_MS=50, --stats): over the case's window of
vblanks, the pictures handed over a second and the game's frames a second (KF4_PRESENT_LOG), and from --stats
for the whole run, a picture: write-backs (each a read-back of a GPU target to VRAM), textures decoded,
pictures between drawn from the packets (a re-recording) and the fallback's cuts (pictures between left out
as the renderer fell behind), and the hitches over 50 ms.
--pictures (--fast): every picture (all phases) of 12 vblanks in the window dumped and compared with
tools/flash_find.py --frames: a picture between that is not between its game frames is flagged (the
under-water and death-water bugs were of this kind). A case with same30 (the wand) also runs at 30 and its game
frames there must be the 240 run's byte for byte: the map change's fade is a loop of its own, every vblank a game
frame, so flash_find has no picture between to judge. The pictures are deleted after (--keep keeps them).

Neither given: both. The verdict: corrupt if a picture is flagged; slow if fewer pictures a second than 90%
of the base case's at the same place (or the game below 29 frames a second); else OK. Run at most two games at a
time on a PC someone plays on; timings shared with other work are noisy.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ISO_DEFAULT = r"E:/roms/ps2/King's Field - The Ancient City (USA).iso"

# the owner's settings (settings.json of 2026-10-24): 240, 3x, native widescreen, MSAA 2, aniso 2, cut-out AA ssaa
OWNER = ["--frame-rate", "240", "--native-scale", "3", "--native-msaa", "2", "--native-aniso", "2",
         "--native-cutout-aa", "ssaa", "--sky", "modern", "--widescreen", "--native-widescreen"]

SPELLS = [3, 6, 9, 12, 16, 21, 31, 36, 46, 51, 61, 66, 76, 81]


def cases():
    """name -> dict(fx, teleport, pad, args, save, spot, base)"""
    c = {}

    def add(name, fx="", teleport=None, pad=None, args=(), spot=None, base="base", window=None, dump=None,
            same30=None):
        c[name] = dict(fx=fx, teleport=teleport, pad=pad or [], args=list(args), spot=spot, base=base,
                       window=window, dump=dump, same30=same30)

    add("base", base=None)
    for fx in ["water", "drain", "drainramp", "prev", "aim", "haze", "depth", "down", "updown", "fill", "fade",
               "tint", "dim", "bright40", "brightff", "copytex"]:
        add(fx, fx)
    add("shift", "shift", args=["--no-whole-frame"])
    add("water+drain", "water,drain")
    add("pause", pad=[(420, "START", 4)])
    add("menu", pad=[(420, "CIRCLE", 4)])
    # the Guardian's Room before its water is purified: the green fill under water
    add("poison base", teleport="35:0", base=None)
    add("poison", "water", teleport="35:0", base="poison base")
    # the King's Map (not usable on maps 000, 002, 004, 008, 009, 098, 099)
    add("kingsmap base", teleport="29:0", base=None)
    add("kingsmap", "mapview=5", teleport="29:0", base="kingsmap base")
    for s in SPELLS:
        add("spell %d" % s, "spell=%d" % s, args=["--cheats", "spells,magic"],
            pad=[(v, "TRIANGLE", 4) for v in range(450, 1400, 120)], dump=(580, 591))
    add("underwater", spot="quick_underwater", pad=[(250, "UP", 60)], base=None, window=(420, 740), dump=(400, 411))
    add("deathwater", spot="quick_deathwater", pad=[(250, "UP", 300)], base=None, window=(350, 480),
        dump=(440, 451))
    add("palace", spot="quick_palace", pad=[(200, "UP", 150)], base=None, window=(300, 980), dump=(400, 411))
    # the Green Wand on a Symbol of Passage: MapChange_CaptureFrame copies the last frame to 0x2BC0 and the map
    # change's loop fades it out (vblanks 740-749), then the new map. Its pictures between used to leave the
    # textures they uploaded there (NATIVE_DRAW_P8.md, "The wand's passage")
    add("wand", spot="quick_wand", pad=[(500, "CIRCLE", 4), (560, "CROSS", 4), (620, "UP", 4), (640, "UP", 4),
                                        (660, "UP", 4), (680, "UP", 4), (720, "CROSS", 4)],
        base=None, window=(700, 800), dump=(738, 749), same30=(740, 749))
    # the icy corridor (map 017, the ice creatures): standing still where the owner's game fell to 11 frames a second.
    # Their shine drawn whole (the default), and exact, in runs (--native-shine exact: the PS2's fold edges, slower)
    add("icy", spot="quick_icy", base=None, window=(300, 980), dump=(400, 411))
    add("icy exact", spot="quick_icy", args=["--native-shine", "exact"], base=None, window=(300, 980), dump=(400, 411))
    # the Dark Succubus' room (map 038), standing still at its entrance where the owner's game fell to 15 frames a
    # second: her body is drawn with the screen copied to 0x3540 (Entity_PrepareScreenTexturedDraw), and its parts
    # each made render passes over the whole frame (NATIVE_DRAW_P8.md, "The Dark Succubus' body"). Up close
    # (KF4_FX=succubus puts the player before her, --invincible as she fights) her body covers much of the screen
    add("succubus", spot="quick_succubus_hotspot", base=None, window=(300, 980), dump=(400, 411))
    add("succubus near", "succubus", spot="quick_succubus_hotspot", args=["--invincible"], base=None,
        window=(160, 400), dump=(190, 201))
    return c


def write_pad(path, presses, offset, start_pad):
    with open(path, "w") as f:
        if start_pad:
            with open(start_pad) as s:
                f.write(s.read())
                f.write("\n")
        else:
            f.write("100 CROSS 4\n")
        for v, b, n in presses:
            f.write("%d %s %d\n" % (v + offset, b, n))


def stat(text, pattern, default=0.0):
    m = re.search(pattern, text)
    return float(m.group(1)) if m else default


def run_case(a, name, case, mode, out):
    d = os.path.join(out, re.sub(r"[^a-z0-9]+", "_", name.lower()) + "_" + mode)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "data"))
    offset = 0 if (a.save or case["spot"]) else 1300
    pad = os.path.join(d, "case.pad")
    write_pad(pad, case["pad"], offset, None if (a.save or case["spot"]) else os.path.join(ROOT, "tests", "newgame.pad"))
    frames = (case["window"][1] + 20 if case["window"] else a.frames) + offset
    window = case["window"] or (500, a.frames - 10)
    window = (window[0] + offset, window[1] + offset)
    dump = case["dump"] or (600, 611)
    dump = (dump[0] + offset, dump[1] + offset)
    cmd = [a.exe, "--disc", a.iso, "--headless", "--memcard", "none", "--no-audio", "--input", pad,
           "--frames", str(frames), "--renderer", a.renderer] + OWNER + case["args"] + a.extra
    if case["spot"]:
        cmd += ["--quick-load", os.path.join(a.spots, case["spot"] + ".kf4quick")]
    elif a.save:
        cmd += ["--quick-load", a.save]
    if case["teleport"]:
        cmd += ["--teleport", "%d:%s" % (300 + offset, case["teleport"])]
    env = dict(os.environ, KF4_DATA_DIR=os.path.join(d, "data"))
    for k in ["KF4_FX", "KF4_PRESENT_SIM", "KF4_PRESENT_LOG", "KF4_GSN_GPU_TIME", "KF4_HITCH_MS"]:
        env.pop(k, None)
    if case["fx"]:
        env["KF4_FX"] = case["fx"]
    if mode == "cost":
        env.update(KF4_INTERP_DRAW_ALL="1")
        cmd += ["--fast", "--stats", "--dump-frames", str(frames - 1), "--dump-dir", d]
    elif mode == "perf":
        if a.pipelines and os.path.isfile(a.pipelines):
            shutil.copy(a.pipelines, os.path.join(d, "data", "native_gs_pipelines.bin"))
        env.update(KF4_PRESENT_SIM="240", KF4_PRESENT_LOG=os.path.join(d, "present.log"), KF4_GSN_GPU_TIME="2",
                   KF4_HITCH_MS="50")
        cmd += ["--stats", "--dump-frames", str(frames - 1), "--dump-dir", d]
    else:
        cmd += ["--fast", "--dump-frames", ",".join(str(v) for v in range(dump[0], dump[1] + 1)), "--dump-dir", d]
    with open(os.path.join(d, "out.txt"), "w") as f:
        rc = subprocess.call(cmd, env=env, stdout=f, stderr=subprocess.STDOUT)
    if mode == "perf" and a.pipelines and os.path.isfile(os.path.join(d, "data", "native_gs_pipelines.bin")):
        shutil.copy(os.path.join(d, "data", "native_gs_pipelines.bin"), a.pipelines)
    shutil.rmtree(os.path.join(d, "data"), ignore_errors=True)
    text = open(os.path.join(d, "out.txt"), errors="replace").read()
    r = {"rc": rc}
    if mode == "cost":
        frames_drawn = stat(text, r"pictures between on the GPU: (\d+) drawn from the packets", 1) or 1
        r["gif_ms"] = stat(text, r"native GS time: GIF ([\d.]+) s") * 1000 / frames_drawn
        r["pkt_ms"] = stat(text, r"drawn from the packets \([\d.]+ s, ([\d.]+) us each") / 1000
        r["rec_ms"] = stat(text, r"from the recording \([\d.]+ s, ([\d.]+) us each") / 1000
        r["c_wb"] = stat(text, r"copies, (\d+) write-backs")
        r["c_dec"] = stat(text, r"write-backs; (\d+) textures decoded")
        r["wall"] = stat(text, r"\[stats\] wall ([\d.]+) s")
        for f in os.listdir(d):
            if f.endswith(".png"):
                os.remove(os.path.join(d, f))
    elif mode == "perf":
        rows = []
        if os.path.isfile(os.path.join(d, "present.log")):
            for line in open(os.path.join(d, "present.log")):
                p = line.split()
                if p and p[0] != "S":
                    rows.append((float(p[0]), int(p[1])))
        w = [x for x in rows if window[0] <= x[1] <= window[1]]
        if len(w) > 1 and w[-1][0] > w[0][0]:
            span = w[-1][0] - w[0][0]
            r["pps"] = len(w) / span
            r["gfps"] = (w[-1][1] - w[0][1]) / 2 / span
        pictures = stat(text, r"native GS: (\d+) pictures;", 1) or 1
        r["wb"] = stat(text, r"copies, (\d+) write-backs") / pictures
        r["dec"] = stat(text, r"write-backs; (\d+) textures decoded") / pictures
        r["rerec"] = stat(text, r"pictures between on the GPU: (\d+) drawn from the packets")
        r["cut"] = stat(text, r"behind the game: (\d+);")
        r["cuts"] = stat(text, r"cut to fewer a vblank (\d+) times")
        r["hitch"] = len(re.findall(r"^\[hitch\]", text, re.M))
        r["gpu"] = stat(text, r"GPU time: [\d.]+ ms in all, ([\d.]+) ms a picture")
        for f in os.listdir(d):
            if f.endswith(".png"):
                os.remove(os.path.join(d, f))
    else:
        ff = subprocess.run([sys.executable, os.path.join(HERE, "flash_find.py"), d, "--frames", "--sheet",
                             os.path.join(d, "flagged.png")], capture_output=True, text=True)
        open(os.path.join(d, "flash_find.txt"), "w").write(ff.stdout + ff.stderr)
        m = re.search(r"(\d+) pictures, (\d+) the same as the one before, (\d+) flagged", ff.stdout)
        r["pics"], r["same"], r["flagged"] = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (0, 0, -1)
        worst = re.findall(r"off\s+([\d.]+)%", ff.stdout)
        r["worst"] = max(float(x) for x in worst) if worst else 0.0
        if case["same30"]:
            # the same run at 30: its game frames must be the 240 run's
            from PIL import Image
            lo, hi = case["same30"][0] + offset, case["same30"][1] + offset
            d30 = os.path.join(d, "at30")
            os.makedirs(os.path.join(d30, "data"))
            cmd30 = [x for x in cmd]
            cmd30[cmd30.index("--frame-rate") + 1] = "30"
            cmd30[cmd30.index("--dump-frames") + 1] = ",".join(str(v) for v in range(lo, hi + 1))
            cmd30[cmd30.index("--dump-dir") + 1] = d30
            with open(os.path.join(d30, "out.txt"), "w") as f:
                subprocess.call(cmd30, env=dict(env, KF4_DATA_DIR=os.path.join(d30, "data")), stdout=f,
                                stderr=subprocess.STDOUT)
            shutil.rmtree(os.path.join(d30, "data"), ignore_errors=True)
            differ = 0
            for v in range(lo, hi + 1):
                name = "frame_%06d.png" % v
                p240, p30 = os.path.join(d, name), os.path.join(d30, name)
                if not (os.path.isfile(p240) and os.path.isfile(p30)):
                    differ += 1
                    continue
                with Image.open(p240) as x, Image.open(p30) as y:
                    differ += x.size != y.size or x.convert("RGB").tobytes() != y.convert("RGB").tobytes()
            r["same30"] = "%d of %d differ" % (differ, hi - lo + 1)
            r["flagged"] += differ
            if not a.keep:
                shutil.rmtree(d30, ignore_errors=True)
        if not a.keep:
            for f in os.listdir(d):
                if f.endswith(".png") and f != "flagged.png":
                    os.remove(os.path.join(d, f))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(ROOT, "out", "windows-clang", "kf4.exe"))
    ap.add_argument("--iso", default=os.environ.get("KF4_DISC", ISO_DEFAULT))
    ap.add_argument("--out", required=True, help="a scratch folder for the runs")
    ap.add_argument("--save", help="the quick save the forced cases start from (default: a new game)")
    ap.add_argument("--spots", help="a folder with the real scenes' quick saves")
    ap.add_argument("--cases", help="comma-separated case names (default: all); 'list' lists them")
    ap.add_argument("--perf", action="store_true")
    ap.add_argument("--pictures", action="store_true")
    ap.add_argument("--cost", action="store_true", help="--fast with every picture between drawn: the GS thread's time a "
                    "game frame (GIF), a picture between from the packets and from the recording, write-backs and "
                    "decodes in all; robust to a GPU shared with other work")
    ap.add_argument("--frames", type=int, default=1000, help="vblanks a forced case runs (1000)")
    ap.add_argument("--renderer", default="modern")
    ap.add_argument("--keep", action="store_true", help="keep the --pictures dumps")
    ap.add_argument("extra", nargs="*", help="more kf4 options (after --)")
    a = ap.parse_args()
    a.exe = os.path.abspath(a.exe)
    a.pipelines = os.path.join(a.out, "pipelines.bin")
    all_cases = cases()
    if a.cases == "list":
        print("\n".join(all_cases))
        return
    names = [n.strip() for n in a.cases.split(",")] if a.cases else list(all_cases)
    for n in names:
        if n not in all_cases:
            sys.exit("no case %r (--cases list)" % n)
    modes = [m for m, on in (("perf", a.perf), ("pictures", a.pictures), ("cost", a.cost)) if on] or ["perf", "pictures"]
    os.makedirs(a.out, exist_ok=True)
    results = {}
    for n in names:
        c = all_cases[n]
        if c["spot"] and not (a.spots and os.path.isfile(os.path.join(a.spots, c["spot"] + ".kf4quick"))):
            print("%-14s left out: no %s.kf4quick in --spots" % (n, c["spot"]), flush=True)
            continue
        results[n] = {}
        for m in modes:
            results[n].update(run_case(a, n, c, m, a.out))
        r = results[n]
        print("%-14s %s" % (n, "  ".join("%s %s" % (k, ("%.2f" % v) if isinstance(v, float) else v)
                                          for k, v in r.items())), flush=True)

    print()
    print("| case | pictures/s | game fps | write-backs/pic | decodes/pic | re-recordings | cut (left out) | "
          "hitches | GPU ms/pic | flagged pictures between (worst %) | GS ms a frame, a picture between "
          "(packets, recording) | verdict |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for n, r in results.items():
        base = results.get(all_cases[n]["base"] or "", {})
        verdict = "OK"
        if r.get("flagged", 0) > 0:
            verdict = "corrupt"
        elif "pps" in r and (r.get("gfps", 30) < 29 or ("pps" in base and r["pps"] < 0.9 * base["pps"])):
            verdict = "slow"
        elif "gif_ms" in r and "gif_ms" in base and (r["gif_ms"] > 1.5 * base["gif_ms"] + 2 or
                                                     r["pkt_ms"] > 1.5 * base["pkt_ms"] + 2 or r["c_wb"] > base["c_wb"] + 10):
            verdict = "slow"
        elif "perf" in modes and "pps" not in r:
            verdict = "no data"

        def f(k, fmt):
            return fmt % r[k] if k in r else ""

        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            n, f("pps", "%.1f"), f("gfps", "%.1f"), f("wb", "%.2f"), f("dec", "%.2f"), f("rerec", "%d"),
            ("%d (%d)" % (r["cuts"], r["cut"])) if "cuts" in r else "", f("hitch", "%d"), f("gpu", "%.2f"),
            ("%d of %d (%.2f)" % (r["flagged"], r["pics"], r["worst"])) if "flagged" in r else "",
            ("%.1f (%.1f, %.1f)" % (r["gif_ms"], r["pkt_ms"], r["rec_ms"])) if "gif_ms" in r else "", verdict))


if __name__ == "__main__":
    main()
