#!/usr/bin/env python3
"""The game's full-screen layers: every one a tour of play draws, its GS state, its GPU cost, what reads it, and
whether it is a clear, dead, foldable into scanout or must stay in memory (docs/plans/fullscreen-layers-inventory.md).

    tools/layer_inventory.py --out DIR [--disc ISO] [--scenarios NAME,...|list|all] [--maps all|N,N,...]
                             [--steps capture,time,live,report] [--scales 3x2,2x1,6x4] [--repeat N] [--jobs N]
                             [--live-rates 60,30] [--live-seconds S]

Steps (each skips what its outputs show done, so a tour can be run again in parts; --force does it again):
- capture: each scenario played headless (native draw off: the GS stream as the PS2 had it, every pass a GIF
  packet), with a GS capture of 4 vblanks at each of its moments (two game frames, so that the next frame's reads
  show), kf4-gsreplay --list --events of each, and a picture of each moment. The scenarios are a New Game and pad
  scripts (tests/newgame.pad, tests/motion/), teleports, KF4_FX (game/screen_fx.c) and quick saves made up for a
  map's start (tools/map_tour.py's); --maps adds one per map (a quick load and a walk).
- time: each capture drawn again by the PC renderer at the player's scale (--scales; 3x2 is 2x with native
  widescreen, the owner's: 1920x896 a buffer), FXAA, 16x anisotropic, --repeat times, with kf4-gsreplay --gpu-draws:
  every GPU interval with the --list draws it was for, so each draw's GPU time (the median over the repeats).
- live: each scenario played in a real window with the owner's settings (KF4_READ_SETTINGS=1, a copy of the data
  folder: CLAUDE.md, "Testing graphics"; native draw on as the owner has it), at each --live-rates frame rate,
  with a profile (--profile-at, KF4_GPU_DETAIL=1): the GPU's time a picture and a game frame by kind and by shader
  state, and the run's [stats]. These are the real totals; the time step says which layer each ms is.
- report: the layers found in every capture (a run of draws in one state that covers 90% or more of a buffer;
  copies of the frame), what each writes (RGB, alpha, Z), what reads each channel later (in its frame, the next
  frame, a copy, the scanout) and so its class; then report.md (the summary tables) and report.json.

Raw output (captures, pictures, profiles) holds the game's data: --out must be outside the repository.
"""
import argparse
import collections
import concurrent.futures
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import map_tour  # noqa: E402  (the disc, the map files' places, made-up quick saves)

NEWGAME = os.path.join(ROOT, "tests", "newgame.pad")
MOTION = os.path.join(ROOT, "tests", "motion")
CLOCK = "2026-11-18T18:00:00"
CAPTURE_VBLANKS = 4

# ---- The scenarios ----------------------------------------------------------------------------------
# name -> what it shows; how it starts ("newgame": tests/newgame.pad, play from about vblank 1500; "map:N": a quick
# save made up at map N's start, loaded on the title screen, play from about vblank 230); pad lines (more input,
# vblanks from the run's start) or a tests/motion pad; args; KF4_FX; env; captures (vblanks, each 4 vblanks long);
# live (the vblank the real-window profile starts at; None: no live run); forced (the scene is forced by KF4_FX or
# an option, not reached in play).
def S(what, start="newgame", pad=None, args=(), fx="", env=None, captures=(), live=None, forced=False,
      invincible=True, frames=None, settings=None):
    return dict(what=what, start=start, pad=pad, args=list(args), fx=fx, env=env or {}, captures=list(captures),
                live=live, forced=forced, invincible=invincible, frames=frames, settings=settings or {})


TELEPORT = "1800:%d:%d"
WALK = "1950 ly=0 120\n2080 rx=255 60\n2150 ry=0 30\n2190 ry=255 30\n"  # walk, turn, look down and up
STAND_WALK = "1600 ly=0 100\n1750 rx=255 80\n"

SCENARIOS = {
    # ---- outside play
    "title": S("the logos, the title, Press Start, New Game, the opening movie's end, the fade into play",
               pad="", captures=[300, 420, 520, 760, 1450], invincible=False),
    # ---- the first map (099), standing, walking, turning
    "start": S("New Game's first map (099), standing", captures=[1800], live=1800),
    "start native off": S("099 standing, the owner's settings but native draw off (the GS stream's passes and DATE "
                          "drawn as GIF batches): against \"start\", what native draw's passes cost", captures=[1800],
                          live=1800, settings={"native_draw": "off"}),
    "start spin": S("099, a mouse spin (KF4_DIAG_SPIN=6: through instant mouse look, as a mouse)",
                    env={"KF4_DIAG_SPIN": "6"}, captures=[1800], live=1800),
    "walk": S("099, walking, a stick turn, looking down and up", pad=STAND_WALK + "1900 ry=0 40\n1950 ry=255 40\n",
              env={"KF4_TWIN_STICK_SCRIPTED": "1"}, captures=[1620, 1780, 1915], live=1600),
    "pickup": S("099, walking to the herb, picking it up, its message box and the message dim",
                pad="@herb_pickup", captures=[1600, 1832, 1860, 1930], live=1780),
    "pause": S("099, the pause (START): the shade and the paused HUD", pad="1850 START 4\n2400 START 4\n",
               captures=[1900], live=1900),
    "menus": S("every page of the menu (tests/motion/menus.pad): its backdrop, the grab, the pages",
               pad="@menus", args=["--cheats", "all"], captures=[1805, 1820, 1910, 2450, 2840, 3500, 3725, 3760],
               live=1900),
    # ---- the screen options
    "bright40": S("099, the brightness option at 0x40", fx="bright40", captures=[1800], live=1800, forced=True),
    "brightff": S("099, the brightness option at 0xFF", fx="brightff", captures=[1800], live=1800, forced=True),
    "flicker": S("099, --flicker-filter (Game_BlendScreenOffsetDown, MapDrawState bit 42)", args=["--flicker-filter"],
                 captures=[1800], live=1800),
    # ---- the screen effects, forced at 099 (KF4_FX: each set for the passes only)
    **{"fx " + name: S("099, KF4_FX=%s: %s" % (name, what), fx=name, captures=[1800], live=1800, forced=True, **kw)
       for name, what, kw in [
           ("water", "under water: the frame's copy to 0x2BC0, the wobbling screen, the tint", {}),
           ("drain", "the MP drain's purple wobbling screen, full strength", {}),
           ("prev", "the frame before blended in (MapDrawState bit 40)", {}),
           ("aim", "the knock-back's frame before (aim_speed)", {}),
           ("haze", "the haze layers (bit 44; no code of the game sets it)", {}),
           ("depth", "the debug depth sprite (bit 41)", {}),
           ("updown", "the one-row blends down and up (bit 43)", {}),
           ("shift", "the view-turn shift (bit 45, --no-whole-frame)", {"args": ["--no-whole-frame"]}),
           ("fill", "MapDrawState's screen fill (red, 0x40)", {}),
           ("fade", "PlayerState.screen_fade (white, 0x40)", {}),
           ("tint", "player_screen_tint's purple and darkness", {}),
           ("dim", "the message dim", {}),
           ("copytex", "the Dark Succubus' screen copy into 0x3540 and a sprite from it", {}),
       ]},
    # ---- places
    "pond": S("009, the swamp: the reflection's view into 0x2BC0 with its Z in the frame's Z buffer",
              pad=WALK, args=["--teleport", TELEPORT % (9, 0)], captures=[2000, 2100], live=2000),
    "forest": S("014, the Heart of the Forest: the reflection drawn 256 wide over the frame's Z",
                pad=WALK, args=["--teleport", TELEPORT % (14, 0)], captures=[2000, 2100], live=2000),
    "canal water": S("029, the Underground Canal, under water forced (KF4_FX=water)", fx="water",
                     args=["--teleport", TELEPORT % (29, 0)], captures=[2000], live=2000, forced=True),
    "guardian water": S("035, the Guardian's Room, under water forced: the green fill instead of the tint",
                        fx="water", args=["--teleport", TELEPORT % (35, 0)], captures=[2000], live=2000, forced=True),
    "palace": S("022, the Queen's Palace, before a Watcher (its MP drain: the purple wobble, when it drains)",
                pad="", fx="creature=30:700", args=["--teleport", TELEPORT % (22, 0)],
                captures=[2100, 2300, 2500], live=2100),
    "giant leader": S("034, the MP drain forced (KF4_FX=drainramp: its colour and wobble changing)",
                      fx="drainramp", args=["--teleport", TELEPORT % (34, 0)], captures=[2000, 2016], live=2000,
                      forced=True),
    "succubus": S("038, the Dark Sanctuary, KF4_FX=copytex (the Dark Succubus' screen copy; she sleeps in a new game)",
                  fx="copytex", args=["--teleport", TELEPORT % (38, 0)], captures=[2000], live=2000, forced=True),
    "flesh": S("039, the flesh cavern: the wave UVs and scrolling textures", pad=WALK,
               args=["--teleport", TELEPORT % (39, 0)], captures=[2000], live=2000),
    "icy": S("017, the icy corridor", pad=WALK, args=["--teleport", TELEPORT % (17, 0)], captures=[2000],
             live=2000),
    "salamanders": S("016, before the salamanders (their fire)", fx="creature=16:900",
                     args=["--teleport", "1800:16:2"], captures=[2100, 2300], live=2100),
    "fire elementals": S("102, before the fire elementals", fx="creature=67:700",
                         args=["--teleport", TELEPORT % (102, 0)], captures=[2100, 2300], live=2100),
    "fire genie": S("025, before the fire genie", fx="creature=74:800", args=["--teleport", TELEPORT % (25, 0)],
                    captures=[2100, 2300], live=2100),
    # ---- combat and magic
    "combat": S("002, the club put on, a Maneater before the player: swings (the weapon's trail), its hits, "
                "the red flash, the knock-back", pad="@combat", args=["--cheats", "all", "--teleport", "2200:2:0"],
                fx="creature=0:400", captures=[2210, 2295, 2510, 2650, 2810], live=2500),
    "first kill": S("002, a Fireball kills the Maneater: its death and the reward", pad="@first_kill",
                    args=["--cheats", "all", "--teleport", "2200:2:0"], fx="creature=0:180,spell=3",
                    captures=[2440, 2600, 2645], live=2440),
    "death": S("002, a Maneater before the player, not invincible: hits until the player's death and the death "
               "screen", pad="", args=["--teleport", "1800:2:0"], fx="creature=0:150", invincible=False,
               captures=[3600, 3860, 3920, 3980], live=None, frames=4100),
    **{"spell %d" % n: S("099, spell %d cast (KF4_FX=spell=%d, tests/motion/cast.pad)" % (n, n), pad="@cast",
                         args=["--cheats", "spells,magic"], fx="spell=%d" % n, captures=[1905, 1925, 2030],
                         live=1900 if n in (3, 16) else None)
       for n in (3, 16, 36, 61, 76, 81)},
    # ---- the map viewers
    **{"mapview %d" % n: S("the map viewer of map item %d%s" % (n, " (the King's Map, at 029)" if n == 5 else ""),
                           fx="mapview=%d" % n, args=["--teleport", TELEPORT % (29, 0)] if n == 5 else [],
                           captures=[1900, 1990] if n != 5 else [2100, 2190], live=None, forced=True)
       for n in (1, 5)},
    # ---- map changes
    "map change": S("099 to 002 by the game's map change (KF4_FX=mapchange=2, as a door with no link): the frame "
                    "captured to 0x2BC0, its fade (its own loop), the new map's fade in", fx="mapchange=2",
                    captures=[1796, 1799, 1802, 1806, 1830, 1880], live=None, forced=True),
    "save point": S("099, the save screen as a save point opens it (KF4_FX=savepoint): its backdrop, its pages",
                    fx="savepoint", pad="1700 DOWN 4\n1760 CROSS 4\n1820 CIRCLE 4\n1880 CIRCLE 4\n",
                    captures=[1700, 1730, 1760, 1800, 1840, 1900], live=None, forced=True),
    "teleport": S("099 to 002 by --teleport (a quick load: the load's fade, not the map change's)",
                  args=["--teleport", "1800:2:0"], captures=[1802, 1812, 1840], live=None),
}


def scenario_names(spec, maps):
    names = list(SCENARIOS) if spec in (None, "", "all") else [s.strip() for s in spec.split(",") if s.strip()]
    names += ["map %03d" % n for n in maps]
    return names


def scenario(name):
    if name in SCENARIOS:
        return SCENARIOS[name]
    m = re.fullmatch(r"map (\d+)", name)
    if m:
        n = int(m.group(1))
        return S("map %03d at its start: a walk and a turn" % n, start="map:%d" % n,
                 pad="300 ly=0 90\n400 rx=255 60\n", captures=[260, 360], live=None)
    sys.exit("layer_inventory: no scenario %r (--scenarios list)" % name)


# ---- Running the game --------------------------------------------------------------------------------
def slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def write_pad(path, sc, made_up):
    lines = []
    if sc["start"] == "newgame":
        lines.append(open(NEWGAME).read())
    else:
        lines.append("100 CROSS 4     # the notice that there is nowhere to save; the made-up save loads on the title\n")
    pad = sc["pad"] or ""
    if pad.startswith("@"):
        lines.append(open(os.path.join(MOTION, pad[1:] + ".pad")).read())
    else:
        lines.append(pad)
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def last_vblank(sc):
    if sc["frames"]:
        return sc["frames"]
    end = max(sc["captures"]) + CAPTURE_VBLANKS + 4
    pad = sc["pad"] or ""
    if pad.startswith("@"):
        text = open(os.path.join(MOTION, pad[1:] + ".pad")).read()
    else:
        text = pad
    for m in re.finditer(r"^(\d+)\s", text, re.M):
        end = max(end, int(m.group(1)) + 10)
    return end


def game_args(a, sc, folder):
    args = list(sc["args"])
    if sc["invincible"]:
        args.append("--invincible")
    if sc["start"].startswith("map:"):
        args += ["--quick-load", made_up_save(a, int(sc["start"][4:]), folder)]
    return args


def made_up_save(a, number, folder):
    """A quick save at map number's start (map_tour's made-up file), from the base save"""
    path = os.path.join(folder, "start.kf4quick")
    if not os.path.exists(path):
        data = a.disc_files.read("%03d.map" % number)
        place = map_tour.map_places(data, 1)[0]
        map_tour.write_made_up(a.base, path, number, place[0], place[1], place[2], place[3])
    return path


def run(cmd, env, log_path, timeout=None):
    started = time.time()
    with open(log_path, "w") as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=env, timeout=timeout)
    return result.returncode, time.time() - started


def capture_scenario(a, name):
    sc = scenario(name)
    folder = os.path.join(a.out, "scenarios", slug(name))
    done = os.path.join(folder, "captured.json")
    if os.path.exists(done) and not a.force:
        return json.load(open(done))
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(folder)
    pad = os.path.join(folder, "input.pad")
    write_pad(pad, sc, None)
    data_dir = os.path.join(folder, "data")
    os.makedirs(data_dir)
    os.makedirs(os.path.join(folder, "pictures"))
    end = last_vblank(sc)
    caps = ",".join("%d:%d" % (v, CAPTURE_VBLANKS) for v in sc["captures"])
    pictures = ",".join(str(v + CAPTURE_VBLANKS - 1) for v in sc["captures"])
    cmd = [a.exe, "--disc", a.disc, "--headless", "--fast", "--no-audio", "--saves", "none", "--clock", CLOCK,
           "--input", pad, "--frames", str(end), "--stats", "--log", os.path.join(folder, "log.txt"),
           "--widescreen", "--native-scale", "2", "--native-widescreen", "--antialias", "fxaa",
           "--gs-capture-at", caps, "--dump-frames", pictures, "--dump-dir", os.path.join(folder, "pictures")]
    cmd += game_args(a, sc, folder)
    env = dict(os.environ, KF4_DATA_DIR=data_dir)
    env.pop("KF4_FX", None)
    if sc["fx"]:
        env["KF4_FX"] = sc["fx"]
    env.update(sc["env"])
    rc, wall = run(cmd, env, os.path.join(folder, "stderr.txt"))
    caps_found = sorted(os.listdir(os.path.join(data_dir, "captures"))) if os.path.isdir(
        os.path.join(data_dir, "captures")) else []
    captures = []
    for f in caps_found:
        m = re.match(r"gs_(\d+)_", f)
        if not m:
            continue
        v = int(m.group(1))
        dest = os.path.join(folder, "capture_%06d.kf4gs" % v)
        shutil.move(os.path.join(data_dir, "captures", f), dest)
        lst = dest[:-6] + ".list.txt"
        with open(lst, "w") as out:
            subprocess.run([a.gsreplay, dest, "--list", "--events"], stdout=out, stderr=subprocess.STDOUT)
        captures.append({"vblank": v, "file": os.path.basename(dest), "list": os.path.basename(lst)})
    shutil.rmtree(data_dir, ignore_errors=True)
    result = {"name": name, "exit": rc, "wall": round(wall, 1), "captures": captures, "cmd": cmd,
              "fx": sc["fx"], "env": sc["env"]}
    json.dump(result, open(done, "w"), indent=1)
    return result


def time_capture(a, folder, cap, scale):
    """The capture drawn again --repeat times at scale with --gpu-draws: its intervals' file"""
    path = os.path.join(folder, cap["file"][:-6] + ".gpu_%s.txt" % scale)
    if os.path.exists(path) and os.path.getsize(path) > 0 and not a.force:
        return path
    cmd = [a.gsreplay, os.path.join(folder, cap["file"]), "--renderer", "pc", "--scale", scale, "--aniso", "16",
           "--lod", "scale", "--2d", "smooth", "--antialias", "fxaa", "--repeat", str(a.repeat), "--gpu-draws", path]
    with open(path[:-4] + ".stderr.txt", "w") as log:
        subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    return path


# ---- The live runs (a real window, the owner's settings) ----------------------------------------------
def live_scenario(a, name, rate, tag="", env_more=None, settings_more=None, args_more=None):
    """The scenario in a real window at rate; tag names a variant's files (live_<tag>_<rate>.*), env_more,
    settings_more and args_more (extra command-line arguments, after the scenario's own, so a later --renderer wins)
    what the variant changes (world_plain.py's switches, scales and renderers)"""
    sc = scenario(name)
    if sc["live"] is None:
        return None
    folder = os.path.join(a.out, "scenarios", slug(name))
    os.makedirs(folder, exist_ok=True)
    stem = "live_%s%d" % (tag + "_" if tag else "", rate)
    prof = os.path.join(folder, stem + ".prof")
    done = os.path.join(folder, stem + ".json")
    if os.path.exists(done) and not a.force:
        return json.load(open(done))
    data_dir = os.path.join(folder, stem.replace("live_", "live_data_", 1))
    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir)
    home = os.path.expanduser("~/.config/darker-reality")
    for f in ("settings.json", "pc_pipeline_list.bin", "pc_pipelines.bin"):
        if os.path.exists(os.path.join(home, f)):
            shutil.copy(os.path.join(home, f), data_dir)
    settings_path = os.path.join(data_dir, "settings.json")
    settings = json.load(open(settings_path))
    settings.setdefault("display", {})["frame_rate"] = rate
    settings["display"].update(sc["settings"])
    settings["display"].update(settings_more or {})
    json.dump(settings, open(settings_path, "w"), indent=1)
    pad = os.path.join(folder, "input.pad")
    write_pad(pad, sc, None)
    start = sc["live"]
    args = game_args(a, sc, folder)
    # A scene with no input after the New Game starts from the base quick save (the New Game's first map, the player
    # in control, saved at 1900) loaded on the title screen: the same scene some 25 s sooner. A teleport at 1800 is
    # then at 300, the profile as long after it.
    quick = sc["start"] == "newgame" and not sc["pad"] and a.base_path and not a.live_newgame
    if quick:
        shift = 1800 - 300
        pad = os.path.join(folder, "live_title.pad")
        with open(pad, "w") as f:
            f.write("100 CROSS 4     # the notice that there is nowhere to save; the base save loads on the title\n")
        args = [re.sub(r"^1800:", "300:", x) for x in args] + ["--quick-load", a.base_path]
        start = max(400, start - shift)
    end = start + a.live_seconds * 60 + 120
    log = os.path.join(folder, stem + "_log.txt")
    cmd = [a.exe, "--disc", a.disc, "--no-audio", "--saves", "none", "--clock", CLOCK, "--input", pad,
           "--frames", str(end), "--stats", "--log", log,
           "--profile-at", "%d:%d:%s" % (start, a.live_seconds, prof)] + args + list(args_more or [])
    env = dict(os.environ, KF4_DATA_DIR=data_dir, KF4_READ_SETTINGS="1", KF4_GPU_DETAIL="1")
    env.pop("KF4_FX", None)
    if sc["fx"]:
        env["KF4_FX"] = sc["fx"]
    env.update(sc["env"])
    env.update(env_more or {})
    rc, wall = run(cmd, env, os.path.join(folder, stem + "_stderr.txt"))
    result = {"name": name, "rate": rate, "exit": rc, "wall": round(wall, 1), "start": start,
              "seconds": a.live_seconds, "from_quick_save": bool(quick),
              "settings": dict(sc["settings"], **(settings_more or {})), "env": dict(sc["env"], **(env_more or {})),
              "fx": sc["fx"], "tag": tag}
    result.update(read_profile(prof))
    result["game_frames"] = game_frames_between(log, start, start + a.live_seconds * 60)
    result["stats"] = [l.rstrip() for l in open(os.path.join(folder, stem + "_stderr.txt"), errors="replace")
                       if l.startswith("[stats] pc renderer GPU time") or l.startswith("[stats] frame rate")
                       or l.startswith("[stats] pc pump:")]
    shutil.rmtree(data_dir, ignore_errors=True)
    json.dump(result, open(done, "w"), indent=1)
    return result


def read_profile(path):
    kinds, shaders, pictures = {}, {}, 0
    if not os.path.exists(path):
        return {"profile": None}
    for line in open(path, errors="replace"):
        p = line.split(" ", 4)
        if p[0] == "G" and len(p) == 5:
            (kinds if p[3] == "kind" else shaders)[p[4].rstrip("\n")] = (int(p[1]), int(p[2]))
        elif p[0] == "P":
            pictures = int(p[1])
    return {"profile": os.path.basename(path), "pictures": pictures, "kinds": kinds, "shaders": shaders}


def game_frames_between(log, v0, v1):
    states = {}
    if os.path.exists(log):
        for line in open(log, errors="replace"):
            m = re.match(r"^(\d+) frame=(\d+)", line)
            if m:
                states[int(m.group(1))] = int(m.group(2))
    a = [states[v] for v in sorted(states) if v >= v0][:1]
    b = [states[v] for v in sorted(states) if v <= v1][-1:]
    return b[0] - a[0] if a and b else None


# ---- Reading a capture's list ------------------------------------------------------------------------
DRAW = re.compile(r"^\s*(\d+) rec\s+(\d+) p(\d) (?:prog (\w+) kick\s+(\d+) |\s+)(\w+)((?: tex| abe| fog)*) ctx(\d) "
                  r"v\s*(\d+) k\s*(\d+) box\s*(-?\d+),\s*(-?\d+)-\s*(-?\d+),\s*(-?\d+) z (\w+)-(\w+) \| (.*)$")
EVENT = re.compile(r"^event (\w+) rec\s+(\d+) before\s+(\d+) (.*)$")


class Draw:
    __slots__ = ("i", "path", "prog", "prim", "tex", "abe", "fog", "ctx", "v", "box", "z", "f", "frame", "zbuf",
                 "test", "alpha", "fix", "fba", "pabe", "tex0", "scissor", "layer", "uv", "xyoffset")


def parse_list(path):
    draws, events = [], []
    for line in open(path, errors="replace"):
        m = DRAW.match(line)
        if m:
            g = m.groups()
            d = Draw()
            d.i, d.path, d.prog, d.prim = int(g[0]), int(g[2]), g[3], g[5]
            flags = g[6].split()
            d.tex, d.abe, d.fog = "tex" in flags, "abe" in flags, "fog" in flags
            d.ctx, d.v = int(g[7]), int(g[8])
            d.box = tuple(int(x) for x in g[10:14])
            d.z = (int(g[14], 16), int(g[15], 16))
            f = {}
            for part in g[16].split(" | "):
                t = part.split()
                if not t:
                    continue
                if t[0] in ("TEX0", "TEX1", "CLAMP"):
                    for j in range(0, len(t) - 1, 2):
                        f[t[j]] = t[j + 1]
                elif t[0] == "SCISSOR":
                    f["SCISSOR"] = t[1]
                    if len(t) >= 4 and t[2] == "XYOFFSET":
                        f["XYOFFSET"] = t[3]
                elif t[0] in ("UV", "RGBA"):
                    f[t[0]] = t[1]
                elif t[0] == "DTHE":
                    f["DTHE"], f["COLCLAMP"] = t[1], t[3] if len(t) > 3 else "?"
                elif t[0] == "ALPHA":
                    f["ALPHA"] = t[1]
                    for j in range(2, len(t) - 1, 2):
                        f["ALPHA." + t[j]] = t[j + 1]
                else:
                    for j in range(1, len(t) - 1, 2):
                        f[t[0] + "." + t[j]] = t[j + 1]
            d.f = f
            d.frame = (int(f["FRAME.fbp"]), int(f["FRAME.fbw"]), int(f["FRAME.psm"], 16), int(f["FRAME.fbmsk"], 16))
            d.zbuf = (int(f["ZBUF.zbp"]), int(f["ZBUF.psm"], 16), int(f["ZBUF.zmsk"]))
            d.test = {k: int(f["TEST." + k], 16 if k == "aref" else 10)
                      for k in ("ate", "atst", "aref", "afail", "date", "datm", "zte", "ztst")}
            al = f["ALPHA"]
            d.alpha = tuple(int(c) for c in al)
            d.fix = int(f["ALPHA.fix"], 16)
            d.fba, d.pabe = int(f["ALPHA.fba"]), int(f["ALPHA.pabe"])
            d.tex0 = int(f["TEX0"], 16)
            s = [int(x) for x in re.split(r"[,-]", f["SCISSOR"])]
            d.scissor = (s[0], s[1], s[2], s[3])
            d.layer = None
            m2 = re.fullmatch(r"(-?[\d.e+]+),(-?[\d.e+]+)-(-?[\d.e+]+),(-?[\d.e+]+)", f.get("UV", ""))
            d.uv = tuple(float(x) for x in m2.groups()) if m2 else None
            d.xyoffset = tuple(float(x) for x in f.get("XYOFFSET", "0,0").split(","))
            draws.append(d)
            continue
        m = EVENT.match(line)
        if m:
            kind, rec, before, rest = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)
            t = rest.split()
            e = {"kind": kind, "rec": rec, "before": before}
            if kind == "vblank":
                e.update(vblank=int(t[1]), field=int(t[3]), dispfb1=(int(t[6]), int(t[8]), int(t[10], 16)),
                         dispfb2=(int(t[13]), int(t[15]), int(t[17], 16)))
            else:
                kv = dict(zip(t[0::2], t[1::2]))
                e.update(dir=int(kv["dir"]), sbp=int(kv["sbp"]), sbw=int(kv["sbw"]), spsm=int(kv["spsm"], 16),
                         dbp=int(kv["dbp"]), dbw=int(kv["dbw"]), dpsm=int(kv["dpsm"], 16),
                         src=tuple(int(x) for x in kv["src"].split(",")),
                         dst=tuple(int(x) for x in kv["dst"].split(",")),
                         size=tuple(int(x) for x in kv["size"].split("x")))
            events.append(e)
    return draws, events


def parse_times(path, draw_count):
    """{draw: ms} for draws' own work (their batches and the copies made for them) and {draw: ms} for other work
    tagged with the draw it came before (local copies, uploads), the median over the repeats after the first two"""
    reps, cur, last = [], None, -1
    if not path or not os.path.exists(path):
        return {}, {}, 0
    for line in open(path):
        if line.startswith("#"):
            continue
        t = line.split()
        if len(t) < 7:
            continue
        work = " ".join(t[2:-4])
        state, first, last_draw, ns = t[-4], int(t[-3]), int(t[-2]), int(t[-1])
        if cur is None or first < last - max(8, draw_count // 2):
            cur = (collections.defaultdict(float), collections.defaultdict(float))
            reps.append(cur)
        last = first
        own = state != "00000000" or work in ("texture copy", "blend copy", "DATE copy")
        if own:
            # (a batch of several draws: shared by them)
            n = last_draw - first + 1
            for k in range(first, last_draw + 1):
                cur[0][k] += ns / 1e6 / n
        else:
            cur[1][first] += ns / 1e6
    used = reps[2:] if len(reps) > 3 else reps
    if not used:
        return {}, {}, 0
    own_keys = set().union(*(r[0].keys() for r in used))
    other_keys = set().union(*(r[1].keys() for r in used))
    own = {k: statistics.median(r[0].get(k, 0.0) for r in used) for k in own_keys}
    other = {k: statistics.median(r[1].get(k, 0.0) for r in used) for k in other_keys}
    return own, other, len(used)


# ---- GS memory --------------------------------------------------------------------------------------
def psm_bits(psm):
    return {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4, 0x1B: 8, 0x24: 4, 0x2C: 4,
            0x30: 32, 0x31: 24, 0x32: 16, 0x3A: 16}.get(psm, 32)


def page_size(psm):
    """a page's pixels across and down (8 KB)"""
    b = psm_bits(psm)
    return {32: (64, 32), 24: (64, 32), 16: (64, 64), 8: (128, 64), 4: (128, 128)}[b]


def pages(bp_page, fbw, psm, x0, y0, x1, y1):
    """the pages a rectangle (inclusive-exclusive pixels) of a buffer at page bp_page, fbw * 64 pixels wide, covers"""
    pw, ph = page_size(psm)
    width = max(1, fbw) * 64
    cols = max(1, width // pw)
    out = set()
    if x1 <= x0 or y1 <= y0:
        return out
    for py in range(max(0, y0) // ph, (max(y0 + 1, y1) - 1) // ph + 1):
        for px in range(max(0, x0) // pw, (max(x0 + 1, min(x1, width)) - 1) // pw + 1):
            out.add(bp_page + py * cols + px)
    return out


CELL = 8                    # (the cells the analysis tracks writes and reads in: 8 x 8 pixels)


class Grid:
    """A buffer's cells as a bit set (one int), w x h pixels"""
    def __init__(self, w, h):
        self.cw, self.ch = (w + CELL - 1) // CELL, (h + CELL - 1) // CELL

    def rect(self, x0, y0, x1, y1):
        """cells wholly or partly in [x0,x1) x [y0,y1)"""
        x0, y0 = max(0, x0) // CELL, max(0, y0) // CELL
        x1, y1 = min(self.cw * CELL, x1), min(self.ch * CELL, y1)
        if x1 <= x0 * CELL or y1 <= y0 * CELL:
            return 0
        x1, y1 = (x1 + CELL - 1) // CELL, (y1 + CELL - 1) // CELL
        row = ((1 << (x1 - x0)) - 1) << x0
        bits = 0
        for y in range(y0, y1):
            bits |= row << (y * self.cw)
        return bits

    def inner(self, x0, y0, x1, y1):
        """cells wholly in [x0,x1) x [y0,y1)"""
        cx0, cy0 = (max(0, x0) + CELL - 1) // CELL, (max(0, y0) + CELL - 1) // CELL
        cx1, cy1 = min(self.cw, x1 // CELL), min(self.ch, y1 // CELL)
        if cx1 <= cx0 or cy1 <= cy0:
            return 0
        row = ((1 << (cx1 - cx0)) - 1) << cx0
        bits = 0
        for y in range(cy0, cy1):
            bits |= row << (y * self.cw)
        return bits

    def all(self):
        return (1 << (self.cw * self.ch)) - 1


RGB, A, Z = 1, 2, 4
NAMES = {RGB: "RGB", A: "alpha", Z: "Z"}


def chan_text(c):
    return "+".join(NAMES[k] for k in (RGB, A, Z) if c & k) or "-"


# ---- One draw's reads and writes (GS rules) ---------------------------------------------------------
def draw_rect(d):
    """the pixels a draw may touch, [x0, x1) x [y0, y1): its box within its scissor. The box is its vertices' places
    cut to whole pixels; a sprite ending a fraction past a pixel (the brightness pass's 447.5) covers that pixel too
    (the GS samples at whole pixels), so a box at the scissor's last row or column reaches the scissor's edge."""
    s = d.scissor
    x1 = s[2] + 1 if d.box[2] >= s[2] else d.box[2]
    y1 = s[3] + 1 if d.box[3] >= s[3] else d.box[3]
    return (max(d.box[0], s[0]), max(d.box[1], s[1]), min(x1, s[2] + 1), min(y1, s[3] + 1))


def blend_reads_dest(d):
    """(colour, alpha) of the destination the blend reads"""
    if not d.abe:
        return False, False
    a, b, c, dd = d.alpha
    return (a == 1 or b == 1 or dd == 1), c == 1


def blend_is_source(d):
    """the blend gives Cs exactly (or blending off)"""
    if not d.abe:
        return True
    a, b, c, dd = d.alpha
    if a == 0 and b == 2 and dd == 2:   # (Cs - 0) * C + 0: Cs only when C is 1
        return c == 2 and d.fix == 0x80
    if a == 0 and b == 1 and dd == 1:   # (Cs - Cd) * C + Cd
        return c == 2 and d.fix == 0x80
    return False


def blend_keeps_dest(d):
    """the blend gives Cd exactly (Cs * 0 + Cd, or (Cs - Cs) ...): the colour is not changed"""
    if not d.abe:
        return False
    a, b, c, dd = d.alpha
    return dd == 1 and ((c == 2 and d.fix == 0 and (a != 1 and b != 1)) or a == b)


def rw(d):
    """(reads, writes, kills) of the draw's own colour and Z buffers: kills are channels written at every pixel of its
    sprites, whatever was there"""
    t = d.test
    fbmsk = d.frame[3]
    psm = d.frame[2]
    rgb_ok = (fbmsk & 0x00FFFFFF) != 0x00FFFFFF
    a_ok = (fbmsk & 0xFF000000) != 0xFF000000 and psm_bits(psm) == 32
    z_ok = d.zbuf[2] == 0 and t["zte"] == 1 and t["ztst"] != 0
    if t["zte"] == 1 and t["ztst"] == 0:
        return 0, 0, 0
    # the alpha test: which channels pass and fail write
    if not t["ate"] or t["atst"] == 1:
        passing, failing, conditional = RGB | A | Z, None, False
    elif t["atst"] == 0:
        passing, failing, conditional = None, {0: 0, 1: RGB | A, 2: Z, 3: RGB}[t["afail"]], False
    else:
        passing, failing, conditional = RGB | A | Z, {0: 0, 1: RGB | A, 2: Z, 3: RGB}[t["afail"]], True
    mask = (RGB if rgb_ok else 0) | (A if a_ok else 0) | (Z if z_ok else 0)
    writes = ((passing or 0) | (failing or 0)) & mask
    always = (passing if failing is None else failing if passing is None else (passing & failing)) & mask
    reads = 0
    cr, ar = blend_reads_dest(d)
    if cr and writes & RGB:
        reads |= RGB
    if ar and writes & RGB:
        reads |= A
    if t["date"]:
        reads |= A
    if d.pabe and d.abe:
        reads |= A
    if t["zte"] and t["ztst"] in (2, 3):
        reads |= Z
    kills = always
    if t["zte"] and t["ztst"] != 1:
        kills = 0                        # (a depth test may fail)
    if t["date"]:
        kills = 0
    if kills & RGB and not blend_is_source(d):
        kills &= ~RGB
    if blend_keeps_dest(d):
        writes &= ~RGB                   # (the colour written is the colour there)
        kills &= ~RGB
    if d.prim != "sprite" or d.v != 2:
        kills = 0                        # (only a single sprite's pixels are known from its box)
    return reads, writes, kills


def tex_source(d):
    """the texture's base page, width (64s) and PSM"""
    tbp = d.tex0 & 0x3FFF
    tbw = (d.tex0 >> 14) & 0x3F
    psm = (d.tex0 >> 20) & 0x3F
    return tbp, tbw, psm


def tex_rect(d):
    """the texels the draw reads: its UV range (clamped to the texture), the whole texture without one"""
    tbp, tbw, tpsm = tex_source(d)
    tw, th = 1 << ((d.tex0 >> 26) & 0xF), 1 << ((d.tex0 >> 30) & 0xF)
    if d.uv is None:
        return 0, 0, tw, th
    u0, v0, u1, v1 = d.uv
    if u0 < -1 or v0 < -1 or u1 > tw + 1 or v1 > th + 1:
        return 0, 0, tw, th              # (repeated, or past the edge: the whole texture)
    # (a filtered read takes the texel past the edge too: TEX1's MMAG or MMIN linear)
    tex1 = int(d.f.get("TEX1", "0"), 16)
    m = 1 if (tex1 >> 5) & 1 or (tex1 >> 6) & 7 in (1, 3, 5) else 0
    return max(0, int(u0) - m), max(0, int(v0) - m), min(tw, int(math.ceil(u1)) + m), min(th, int(math.ceil(v1)) + m)


def tex_pages(d):
    tbp, tbw, tpsm = tex_source(d)
    x0, y0, x1, y1 = tex_rect(d)
    return pages(tbp // 32, tbw, tpsm, x0, y0, x1, y1)


def tex_channels(d):
    """the texture's channels the draw uses: RGB, and A with TCC (the alpha test and the blend may read it)"""
    tcc = (d.tex0 >> 34) & 1
    return RGB | (A if tcc else 0)


# ---- The analysis ------------------------------------------------------------------------------------
FRAME_W, FRAME_H = 640, 448


class Capture:
    def __init__(self, list_path, own, other):
        self.draws, self.events = parse_list(list_path)
        self.own, self.other = own, other
        self.frames = self.split_frames()
        self.layers = []
        self.small_targets = set()
        self.shown = self.shown_buffers()
        self.complete = self.frames_shown()
        self.find_layers()
        self.mark_same_pixel()

    def mark_same_pixel(self):
        """a layer reading the buffer it draws into, each draw at its own pixels (UV = the box, within a pixel):
        the brightness pass; a one-row blend or a shift is not"""
        for L in self.layers:
            if "transfer" in L:
                continue
            d0 = self.draws[L["first"]]
            if not d0.tex or tex_source(d0)[0] != d0.frame[0] * 32:
                continue
            offs = set()
            for d in self.draws[L["first"]:L["last"] + 1]:
                if d.uv is None:
                    offs.add((99, 99))
                    continue
                offs.add((round(d.uv[0] - d.box[0], 1), round(d.uv[1] - d.box[1], 1)))
                offs.add((round(d.uv[2] - d.box[2], 1), round(d.uv[3] - d.box[3], 1)))
            L["uv_offsets"] = sorted(offs)
            L["same_pixel"] = all(abs(u) <= 1 and abs(v) <= 1 for u, v in offs)

    def split_frames(self):
        """[(first draw, end draw)] of each game frame: the draws between vblanks that drew"""
        cuts = sorted({e["before"] for e in self.events if e["kind"] == "vblank"} | {0, len(self.draws)})
        frames = [(a, b) for a, b in zip(cuts, cuts[1:]) if b > a]
        return frames

    def targets(self):
        """every buffer the capture draws into, tests Z in, copies into, or reads from a buffer drawn into: name ->
        pages, and who uses it"""
        out = {}

        def add(key, pg, how):
            e = out.setdefault(key, {"pages": set(), "how": set()})
            e["pages"] |= pg
            e["how"].add(how)
        drawn = set()
        for d in self.draws:
            r = draw_rect(d)
            key = "colour FBP %d (%d wide, PSM %02X)" % (d.frame[0], d.frame[1] * 64, d.frame[2])
            pg = pages(d.frame[0], d.frame[1], d.frame[2], *r)
            if pg and rw(d)[1] & (RGB | A):
                add(key, pg, "drawn")
                drawn |= pg
            if d.test["zte"] and (d.test["ztst"] in (2, 3) or rw(d)[1] & Z):
                zk = "Z ZBP %d (%d wide, PSM %02X)" % (d.zbuf[0], d.frame[1] * 64, 0x30 | (d.zbuf[1] & 0xF))
                zp = pages(d.zbuf[0], d.frame[1], 0x30 | (d.zbuf[1] & 0xF), *r)
                if zp:
                    add(zk, zp, "Z")
        for e in self.events:
            if e["kind"] == "transfer" and e["dir"] == 2:
                add("copy to 0x%04X (%d wide, PSM %02X)" % (e["dbp"], e["dbw"] * 64, e["dpsm"]),
                    pages(e["dbp"] // 32, e["dbw"], e["dpsm"], e["dst"][0], e["dst"][1], e["dst"][0] + e["size"][0],
                          e["dst"][1] + e["size"][1]), "copy")
        for d in self.draws:
            if d.tex:
                tp = tex_pages(d)
                if tp & drawn:
                    tbp, tbw, tpsm = tex_source(d)
                    add("texture 0x%04X (%d wide, PSM %02X)" % (tbp, tbw * 64, tpsm), tp & drawn, "read as texture")
        return out

    def frames_shown(self):
        """for each game frame, whether a vblank after its last draw shows the buffer it drew most into (else its
        scanout, and what reads it after, are past the capture's end: its layers are not judged)"""
        out = []
        for a, b in self.frames:
            counts = collections.Counter(d.frame[0] for d in self.draws[a:b] if d.frame[0] in self.shown)
            main = counts.most_common(1)[0][0] if counts else None
            out.append(any(e["kind"] == "vblank" and e["before"] >= b and e["dispfb2"][0] == main
                           for e in self.events))
        return out

    def frame_of(self, i):
        for k, (a, b) in enumerate(self.frames):
            if a <= i < b:
                return k
        return len(self.frames)

    def shown_buffers(self):
        return {e["dispfb2"][0] for e in self.events if e["kind"] == "vblank"} | \
               {e["dispfb1"][0] for e in self.events if e["kind"] == "vblank" and e["dispfb1"][1]}

    def key(self, d):
        """what a layer's draws share: all of the state but the box, the vertices and, untextured, TEX0"""
        tex = (tex_source(d), (d.tex0 >> 34) & 7) if d.tex else None
        colour = None if d.tex else d.f.get("RGBA")   # (two fills one after the other in one state: two layers)
        return (d.path, d.prog, d.prim, d.tex, d.abe, d.frame, d.zbuf, tuple(sorted(d.test.items())), d.alpha,
                d.fix, d.fba, d.pabe, tex, colour)

    def find_layers(self):
        groups = []
        for d in self.draws:
            if groups and self.key(groups[-1][-1]) == self.key(d) and groups[-1][-1].i == d.i - 1:
                groups[-1].append(d)
            else:
                groups.append([d])
        for g in groups:
            # (the game's screen passes go through VU1's program 0x276, which passes GIF packets on, or PATH2/3;
            # the world's programs draw meshes, a near wall among them)
            if not all(d.path in (2, 3) or (d.prog or "").upper() == "276" for d in g):
                continue
            if not all(d.prim == "sprite" or d.v <= 6 or (d.prim == "tstrip" and d.prog == "276") for d in g):
                continue
            d0 = g[0]
            fw = max(1, d0.frame[1]) * 64
            s = d0.scissor
            area_w, area_h = s[2] + 1 - s[0], s[3] + 1 - s[1]
            grid = Grid(max(fw, s[2] + 1), s[3] + 1)
            cov = 0
            for d in g:
                cov |= grid.rect(*draw_rect(d))
            whole = grid.rect(s[0], s[1], s[2] + 1, s[3] + 1)
            frac = bin(cov & whole).count("1") / max(1, bin(whole).count("1"))
            is_frame = d0.frame[0] in self.shown and area_w >= FRAME_W - 8
            if frac < 0.9:
                continue
            if not is_frame and area_w * area_h < 256 * 256:
                # (a small target: the scrolling textures' 128 x 128; kept for the shared memory table)
                self.small_targets.add((d0.frame[0], d0.frame[1], d0.frame[2], area_w, area_h))
                continue
            f = self.frame_of(g[0].i)
            layer = {"first": g[0].i, "last": g[-1].i, "draws": len(g), "frame": f,
                     "complete": f < len(self.complete) and self.complete[f],
                     "coverage": round(frac, 3), "target": "frame" if is_frame else "other",
                     "size": [area_w, area_h]}
            for d in g:
                d.layer = len(self.layers)
            self.layers.append(layer)
        for e in self.events:
            if e["kind"] == "transfer" and e["dir"] == 2 and (
                    e["sbp"] // 32 in self.shown or e["size"][0] * e["size"][1] >= 256 * 224):
                f = self.frame_of(e["before"])
                self.layers.append({"transfer": e, "first": e["before"], "last": e["before"] - 1, "draws": 0,
                                    "frame": f, "complete": f < len(self.complete) and self.complete[f],
                                    "coverage": 1.0, "target": "copy",
                                    "size": list(e["size"])})

    # The dataflow: each layer's written channels followed forward to their readers
    def steps(self):
        """the capture's steps in order: ("draw", d) and ("event", e)"""
        evs = sorted(self.events, key=lambda e: (e["before"], e["rec"]))
        out, k = [], 0
        for d in self.draws:
            while k < len(evs) and evs[k]["before"] <= d.i:
                out.append(("event", evs[k]))
                k += 1
            out.append(("draw", d))
        out += [("event", e) for e in evs[k:]]
        return out

    def analyse(self):
        steps = self.steps()
        index = {}
        for n, (kind, x) in enumerate(steps):
            if kind == "draw":
                index[("d", x.i)] = n
            else:
                index[("e", id(x))] = n
        for L in self.layers:
            self.follow(L, steps, index)
            self.state_facts(L)

    def colour_pages(self, d):
        r = draw_rect(d)
        return pages(d.frame[0], d.frame[1], d.frame[2], *r)

    def z_pages(self, d):
        r = draw_rect(d)
        return pages(d.zbuf[0], d.frame[1], 0x30 | (d.zbuf[1] & 0xF), *r)

    def follow(self, L, steps, index):
        if "transfer" in L:
            e = L["transfer"]
            L["state"] = "local copy (BITBLT) %dx%d from page %d (%d wide, PSM %02X) to page %d (%d wide, PSM %02X)" % (
                e["size"][0], e["size"][1], e["sbp"] // 32, e["sbw"] * 64, e["spsm"], e["dbp"] // 32,
                e["dbw"] * 64, e["dpsm"])
            L["writes"] = chan_text(RGB | A)
            L["reads_source"] = "page %d" % (e["sbp"] // 32)
            dst_pages = pages(e["dbp"] // 32, e["dbw"], e["dpsm"], e["dst"][0], e["dst"][1],
                              e["dst"][0] + e["size"][0], e["dst"][1] + e["size"][1])
            readers = []
            start = index[("e", id(e))] + 1
            for kind, x in steps[start:]:
                if kind == "draw" and x.tex:
                    if tex_pages(x) & dst_pages:
                        readers.append(self.reader_text(x, L, "texture"))
                if kind == "draw" and self.colour_pages(x) & dst_pages and rw(x)[1] & (RGB | A):
                    readers.append(self.reader_text(x, L, "written over (draw)"))
                    break
                if kind == "event" and x["kind"] == "transfer" and x["dir"] == 0:
                    p = pages(x["dbp"] // 32, x["dbw"], x["dpsm"], x["dst"][0], x["dst"][1],
                              x["dst"][0] + x["size"][0], x["dst"][1] + x["size"][1])
                    if p & dst_pages:
                        readers.append("upload over page %d (before draw %d)" % (x["dbp"] // 32, x["before"]))
                        break
            L["readers"] = dedupe(readers)
            L["class"] = "d" if any("texture" in r for r in L["readers"]) else "b"
            L["why"] = "a copy read as a texture later" if L["class"] == "d" else "copied and never read"
            return
        d0 = self.draws[L["first"]]
        draws = self.draws[L["first"]:L["last"] + 1]
        reads, writes, kills = 0, 0, RGB | A | Z
        for d in draws:
            r, w, k = rw(d)
            reads |= r
            writes |= w
        # kills: a channel killed over the layer's cells when its draws, each a sprite, kill it
        grid = Grid(max(1, d0.frame[1]) * 64, max(FRAME_H, d0.scissor[3] + 1))
        cells = 0
        kill_cells = {c: 0 for c in (RGB, A, Z)}
        for d in draws:
            r = draw_rect(d)
            cells |= grid.rect(*r)
            _, _, k = rw(d)
            for c in (RGB, A, Z):
                if k & c:
                    kill_cells[c] |= grid.inner(*r)
        L["reads_dest"] = chan_text(reads)
        L["writes"] = chan_text(writes)
        L["kills"] = chan_text(sum(c for c in (RGB, A, Z) if kill_cells[c] and kill_cells[c] | (~cells & grid.all()) == grid.all()))
        if d0.tex:
            tbp, tbw, tpsm = tex_source(d0)
            L["texture"] = self.texture_text(d0)
        # follow each written channel
        own_key = d0.frame[:3]
        live = {c: cells for c in (RGB, A, Z) if writes & c}
        readers = {c: [] for c in live}
        start = index[("d", L["last"])] + 1
        layer_pages_c = set().union(*(self.colour_pages(d) for d in draws))
        layer_pages_z = set().union(*(self.z_pages(d) for d in draws))
        for kind, x in steps[start:]:
            if not any(live.values()):
                break
            if kind == "event":
                if x["kind"] == "vblank":
                    for fb in (x["dispfb2"],) + ((x["dispfb1"],) if x["dispfb1"][1] else ()):
                        if fb[0] == own_key[0] and RGB in live and live[RGB]:
                            readers[RGB].append("scanout (vblank %d)" % x["vblank"])
                    continue
                if x["dir"] in (1, 2):
                    p = pages(x["sbp"] // 32, x["sbw"], x["spsm"], x["src"][0], x["src"][1],
                              x["src"][0] + x["size"][0], x["src"][1] + x["size"][1])
                    for c in list(live):
                        if live[c] and p & (layer_pages_z if c == Z else layer_pages_c):
                            readers[c].append("copy to page %d (before draw %d)%s" % (
                                x["dbp"] // 32, x["before"], self.next_frame_note(L, x["before"])))
                if x["dir"] in (0, 2):
                    p = pages(x["dbp"] // 32, x["dbw"], x["dpsm"], x["dst"][0], x["dst"][1],
                              x["dst"][0] + x["size"][0], x["dst"][1] + x["size"][1])
                    for c in list(live):
                        if live[c] and p & (layer_pages_z if c == Z else layer_pages_c):
                            readers[c].append("written over by a transfer to page %d (partly)" % (x["dbp"] // 32))
                continue
            d = x
            r, w, k = rw(d)
            rect = draw_rect(d)
            same_c = d.frame[:3] == own_key
            # texture reads of the layer's pages
            if d.tex:
                tp = tex_pages(d)
                own_pixel = d.layer is not None and self.layers[d.layer].get("same_pixel") and \
                    tex_source(d)[0] == own_key[0] * 32
                for c in (RGB, A):
                    if c in live and live[c] and tp & layer_pages_c and tex_channels(d) & c:
                        if own_pixel:
                            readers[c].append(self.reader_text(d, L, "own pixel (a layer)"))
                        else:
                            readers[c].append(self.reader_text(d, L, "texture"))
                if Z in live and live[Z] and tp & layer_pages_z:
                    readers[Z].append(self.reader_text(d, L, "texture of Z"))
            # reads through the draw's own buffers
            if same_c:
                dc = grid.rect(*rect)
                for c in (RGB, A):
                    if c in live and live[c] & dc and r & c:
                        readers[c].append(self.reader_text(d, L, "blend" if c == RGB else
                                                           ("DATE" if d.test["date"] else "blend (Ad)")))
            elif self.colour_pages(d) & layer_pages_c:
                for c in (RGB, A):
                    if c in live and live[c] and r & c:
                        readers[c].append(self.reader_text(d, L, "blend through another width"))
            if Z in live and live[Z] and r & Z and self.z_pages(d) & layer_pages_z:
                if d.zbuf[0] == d0.zbuf[0] and d.frame[1] == d0.frame[1]:
                    if live[Z] & grid.rect(*rect):
                        readers[Z].append(self.reader_text(d, L, "depth test"))
                else:
                    readers[Z].append(self.reader_text(d, L, "depth test through another width"))
            # written over in its frame by a draw of no layer (the HUD, the arm): a fold into the scanout would
            # change those pixels too
            if same_c and w & RGB and d.layer is None and RGB in live and live[RGB] & grid.rect(*rect) and \
                    self.frame_of(d.i) == L["frame"] and "overdrawn" not in L:
                L["overdrawn"] = "draw %d %s p%d %s" % (d.i, d.prim, d.path, d.prog or "")
            # kills
            if k:
                inner = grid.inner(*rect)
                for c in (RGB, A):
                    if c in live and k & c and same_c:
                        live[c] &= ~inner
                if Z in live and k & Z and d.zbuf[0] == d0.zbuf[0] and d.frame[1] == d0.frame[1]:
                    live[Z] &= ~inner
        # what is left at the capture's end: the buffer's next use starts as its frame did; what the draws before the
        # layer in its frame killed at every pixel (the frame's clears) is killed then too
        f0 = self.frames[L["frame"]][0] if L["frame"] < len(self.frames) else 0
        start_kills = {c: 0 for c in (RGB, A, Z)}
        for d in self.draws[f0:L["first"]]:
            r, w, k = rw(d)
            if d.frame[:3] == own_key:
                for c in (RGB, A):
                    if k & c:
                        start_kills[c] |= grid.inner(*draw_rect(d))
            if k & Z and d.zbuf[0] == d0.zbuf[0] and d.frame[1] == d0.frame[1]:
                start_kills[Z] |= grid.inner(*draw_rect(d))
        end = {}
        for c in live:
            if live[c] and live[c] & ~start_kills[c]:
                end[c] = "live at the capture's end"
            elif live[c]:
                end[c] = "cleared at the buffer's next use (as its frame began)"
        L["readers"] = {NAMES[c]: dedupe(readers[c]) for c in readers}
        L["end"] = {NAMES[c]: v for c, v in end.items()}
        L["writes_mask"] = writes
        L["kills_mask"] = kills
        L["reads_mask"] = reads

    def next_frame_note(self, L, draw_index):
        f = self.frame_of(draw_index)
        return "" if f == L["frame"] else " [frame +%d]" % (f - L["frame"])

    def texture_text(self, d):
        tbp, tbw, tpsm = tex_source(d)
        bufs = {0: "frame buffer 0 (FBP 0)", 0x1180: "frame buffer 1 (FBP 140)", 0x2BC0: "0x2BC0 (the frame's copy, "
                "the reflection)", 0x3240: "0x3240 (the menus' grab)", 0x3540: "0x3540 (the Succubus' copy)"}
        name = bufs.get(tbp, "TBP 0x%04X" % tbp)
        if tbp in (d.frame[0] * 32,):
            name += ", the buffer being drawn"
        return "%s, %d wide, PSM %02X, TCC %d, TFX %d" % (name, tbw * 64, tpsm, (d.tex0 >> 34) & 1,
                                                          (d.tex0 >> 35) & 3)

    def reader_text(self, d, L, how):
        f = self.frame_of(d.i)
        tag = ""
        if d.layer is not None and d.layer < len(self.layers):
            tag = " (layer %d-%d)" % (self.layers[d.layer]["first"], self.layers[d.layer]["last"])
        nf = "" if f == L["frame"] else " [frame +%d]" % (f - L["frame"])
        return "%s: draw %d %s%s%s%s" % (how, d.i, d.prim, " p%d %s" % (d.path, d.prog or ""), tag, nf)

    def state_facts(self, L):
        if "transfer" in L:
            return
        d = self.draws[L["first"]]
        a, b, c, dd = d.alpha
        cn = ["Cs", "Cd", "0", "?"]
        an = ["As", "Ad", "FIX", "?"]
        L["gs"] = {
            "path": "PATH%d%s" % (d.path, " (VU1 %s)" % d.prog if d.prog else ""),
            "prim": d.prim + (" tex" if d.tex else "") + (" abe" if d.abe else "") + (" fog" if d.fog else ""),
            "frame": "FBP %d, %d wide, PSM %02X, FBMSK %08X" % (d.frame[0], d.frame[1] * 64, d.frame[2], d.frame[3]),
            "zbuf": "ZBP %d, PSM %02X, ZMSK %d" % d.zbuf,
            "test": "ATE %d ATST %d AREF %02X AFAIL %d, DATE %d DATM %d, ZTE %d ZTST %d" % tuple(
                d.test[k] for k in ("ate", "atst", "aref", "afail", "date", "datm", "zte", "ztst")),
            "alpha": "(%s - %s) * %s + %s, FIX %02X%s" % (cn[a], cn[b], an[c], cn[dd], d.fix, "" if d.abe else
                                                         " (blending off)"),
            "fba_pabe": "FBA %d, PABE %d" % (d.fba, d.pabe),
            "dthe_colclamp": "DTHE %s, COLCLAMP %s" % (d.f.get("DTHE", "?"), d.f.get("COLCLAMP", "?")),
        }
        L["signature"] = "%s|%s|%s|%s|%s|%s|%s|%s" % (L["gs"]["path"], L["gs"]["prim"], L["gs"]["test"],
                                                      L["gs"]["alpha"], "ZMSK %d" % d.zbuf[2],
                                                      "FBMSK %08X" % d.frame[3], L.get("texture", "-").split(",")[0]
                                                      if d.tex else "-", L["target"])

    def costs(self, L):
        """GPU ms of the layer at the replay's scale: its draws' own work"""
        if "transfer" in L:
            return self.other.get(L["first"], 0.0)
        return sum(self.own.get(i, 0.0) for i in range(L["first"], L["last"] + 1))


def dedupe(items):
    seen, out = set(), []
    for x in items:
        k = re.sub(r"vblank \d+", "vblank", x)
        k = re.sub(r"draw \d+", "draw", k)
        if k not in seen:
            seen.add(k)
            out.append(x)
    return out[:8]


# ---- Classes ----------------------------------------------------------------------------------------
def classify(L, cap):
    """(a) whole-buffer clear, (b) dead, (c) foldable into scanout, (d) must stay in memory, (e) other"""
    if "transfer" in L:
        return L["class"], L["why"]
    d = cap.draws[L["first"]]
    writes = L["writes_mask"]
    readers = {c: L["readers"].get(NAMES[c], []) for c in (RGB, A, Z)}
    end = L["end"]

    def read(c):
        return bool(readers.get(c)) or NAMES[c] in end and "scanout" not in "".join(readers.get(c, []))

    live = [c for c in (RGB, A, Z) if writes & c and (readers.get(c) or end.get(NAMES[c], "").startswith("live"))]
    if not writes:
        return "b", "writes nothing (masked, or the alpha test drops every pixel)"
    if not live:
        return "b", "every channel it writes (%s) is written over before anything reads it" % chan_text(writes)
    flat = not d.tex and all(cap.draws[i].v == 2 for i in range(L["first"], L["last"] + 1))
    full = L["coverage"] >= 0.99 and L["kills_mask"] == writes and blend_is_source(d)
    if flat and (L["kills"] != "-") and set(L["kills"].split("+")) >= set(chan_text(writes).split("+")) and \
            d.test["zte"] and d.test["ztst"] == 1 and L["coverage"] >= 0.99:
        if writes == A:
            return "e", ("the destination alpha set over the whole buffer, its colour kept: a clear only where alpha "
                         "is an image (or stencil) of its own")
        return "a", "a flat %s over the whole buffer, nothing tested" % chan_text(writes)
    # what reads it
    texture = [r for c in (RGB, A) for r in readers.get(c, []) if r.startswith("texture")]
    if L["target"] == "other":
        return "d", "an offscreen target, read as a texture" if texture else "an offscreen target"
    copies = [r for c in (RGB, A, Z) for r in readers.get(c, []) if r.startswith("copy")]
    later = [r for c in (RGB, A, Z) for r in readers.get(c, []) if "[frame +" in r and not r.startswith("scanout")]
    if texture or copies or later:
        why = (texture + copies + later)[0]
        return "d", "read back: " + why
    colour_only = not (writes & Z and readers.get(Z)) and not (writes & A and readers.get(A))
    per_pixel = (not d.tex) or L.get("same_pixel", False)
    rgb_readers = readers.get(RGB, [])
    only_scanout_or_layers = all(r.startswith("scanout") or "(layer" in r for r in rgb_readers)
    if colour_only and per_pixel and only_scanout_or_layers and not L.get("overdrawn"):
        return "c", "a per-pixel colour op read only by the scanout%s" % (
            " and later layers" if any("(layer" in r for r in rgb_readers) else "")
    reasons = []
    if not per_pixel:
        reasons.append("reads a texture not at its own pixel")
    if not colour_only:
        reasons.append("its %s is read (%s)" % (
            chan_text((A if writes & A and readers.get(A) else 0) | (Z if writes & Z and readers.get(Z) else 0)),
            "; ".join((readers.get(A) or readers.get(Z) or [])[:2])))
    if not only_scanout_or_layers:
        reasons.append("later draws read its colour: " + "; ".join(r for r in rgb_readers
                                                                   if not r.startswith("scanout"))[:200])
    if L.get("overdrawn"):
        reasons.append("drawn over before the scanout by %s (a fold would change those pixels): foldable at its "
                       "place, not into the scanout" % L["overdrawn"])
    return "e", ", ".join(reasons) or "?"


# ---- The steps --------------------------------------------------------------------------------------
def step_capture(a, names):
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        futures = {pool.submit(capture_scenario, a, n): n for n in names}
        for fut in concurrent.futures.as_completed(futures):
            n = futures[fut]
            r = fut.result()
            results[n] = r
            print("capture %-22s exit %d, %d captures, %.0f s" % (n, r["exit"], len(r["captures"]), r["wall"]),
                  flush=True)
    return results


def step_list(a, names):
    """each capture's --list --events made again (a kf4-gsreplay with more in its list)"""
    for n in names:
        folder = os.path.join(a.out, "scenarios", slug(n))
        done = os.path.join(folder, "captured.json")
        if not os.path.exists(done):
            continue
        for c in json.load(open(done))["captures"]:
            with open(os.path.join(folder, c["list"]), "w") as out:
                subprocess.run([a.gsreplay, os.path.join(folder, c["file"]), "--list", "--events"], stdout=out,
                               stderr=subprocess.STDOUT)
            # (the capture's pictures, drawn by paraLLEl-GS at the PS2's size, where the run wrote none)
            pics = os.path.join(folder, "pictures", c["file"][:-6])
            if not os.path.isdir(pics) or not os.listdir(pics):
                os.makedirs(pics, exist_ok=True)
                subprocess.run([a.gsreplay, os.path.join(folder, c["file"]), "--frames", "--out", pics],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def step_time(a, names):
    for n in names:
        folder = os.path.join(a.out, "scenarios", slug(n))
        done = os.path.join(folder, "captured.json")
        if not os.path.exists(done):
            continue
        cap = json.load(open(done))
        scales = a.scales if (n in a.scale_scenarios or not a.scale_scenarios) else a.scales[:1]
        for c in cap["captures"]:
            for scale in scales:
                started = time.time()
                time_capture(a, folder, c, scale)
                print("time %-22s %s at %s: %.0f s" % (n, c["file"], scale, time.time() - started), flush=True)


def step_live(a, names):
    for n in names:
        sc = scenario(n)
        if sc["live"] is None:
            continue
        for rate in a.live_rates:
            r = live_scenario(a, n, rate)
            if r:
                per = (1.0 / r["pictures"]) if r.get("pictures") else 0
                total = sum(v[0] for v in r.get("kinds", {}).values()) / 1e6 * per
                print("live %-22s at %d: %.2f ms a picture, %s game frames in %d s" % (
                    n, rate, total, r.get("game_frames"), a.live_seconds), flush=True)


def layer_name(L, cap):
    """the game function a layer's state says it is (the table in the inventory: docs/plans/)"""
    if "transfer" in L:
        e = L["transfer"]
        return {0x2BC0: "GsDraw_CopyFrame(0x2BC0)", 0x3240: "the menus' grab (0x3240)"}.get(
            e["dbp"], "local copy to 0x%04X" % e["dbp"])
    d = cap.draws[L["first"]]
    t = d.test
    tbp = tex_source(d)[0] if d.tex else None
    fb = d.frame[0] * 32
    shown_other = {0: 0x1180, 0x1180: 0}.get(fb)
    rgba = int(d.f.get("RGBA", "0"), 16)
    colour = (rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF)
    if d.path == 3 and not d.tex and not d.abe and t["ztst"] == 1:
        return "sceGsSwapDBuff's clear (ctx%d): Display_SwapBuffers" % d.ctx
    if not d.tex and t["ate"] and t["atst"] == 0 and t["afail"] == 2:
        return "GsDraw_ClearDepth (Z only)"
    if not d.tex and d.abe and d.alpha == (0, 2, 2, 1) and t["atst"] == 0 and t["afail"] == 1:
        return "GsDraw_SetScreenAlpha (the destination alpha set; Entity_DrawPoolModels)"
    if not d.tex and not d.abe and t["ztst"] == 1 and d.zbuf[2] == 1 and L["target"] == "other":
        return "GsDraw_ClearFrame (an offscreen target's clear, Z kept)"
    if not d.tex and d.abe and L["draws"] == 10:
        blend = "0x44" if d.alpha == (0, 1, 0, 1) else "0x48" if d.alpha == (0, 2, 0, 1) else \
            "ALPHA %s" % "".join(map(str, d.alpha))
        return "GsDraw_FillScreen(0x%02X, 0x%02X, 0x%02X, 0x%02X, %s)%s" % (
            colour + ((rgba >> 24) & 0xFF, blend, ": Game_DrawFade (Z kept)" if d.zbuf[2] == 1 else ""))
    if d.tex and tbp == fb and d.zbuf[2] == 1 and not d.abe and L["draws"] == 10:
        if colour == (0x5E, 0x87, 0x92):
            return "Game_TintScreen(0x5E, 0x87, 0x92) (under water's tint)"
        if colour == (0xEA, 0xB2, 0x46):
            return "Game_TintScreen(0xEA, 0xB2, 0x46) (the King's Map's tint)"
        if colour[0] == colour[1] == colour[2]:
            return "Game_ApplyBrightness (the frame times %d / 128)" % colour[0]
        return "Game_TintScreen(0x%02X, 0x%02X, 0x%02X)" % colour
    if d.tex and tbp == fb and L["draws"] in (10, 20):
        return "a pass reading the frame being drawn (tint, one-row blend or the view-turn shift)"
    if d.tex and shown_other is not None and tbp == shown_other:
        return "Game_BlendPreviousFrame (the shown buffer blended in)"
    if d.tex and tbp == 0x2BC0 and L["draws"] >= 60:
        return "Game_DrawWobblingScreen (from the copy at 0x2BC0)"
    if d.tex and tbp == 0x2BC0:
        return "a pass from 0x2BC0 (the map change's fade of its copy, or the pause/map viewers' picture)"
    if d.tex and tbp == 0x3240 and d.path == 3:
        return "the menu loop's backdrop from the grab at 0x3240 (each menu frame)"
    if d.tex and tbp == 0x3240:
        return "GameLoop_ShowGrabbedFrame (the menus' backdrop at 0x3240)"
    if d.path == 3 and not d.tex and d.abe:
        return "the menu loop's shade over its backdrop (alpha 0x%02X)" % ((rgba >> 24) & 0xFF)
    if fb == 0x3540:
        return "GsDraw_CopyFrameToTexture(0x3540) (the Dark Succubus' screen copy)"
    if fb == 0x3240:
        return "the shown frame grabbed into 0x3240 (Menu_CopyFrameToTexture and the like)"
    if d.tex and tbp == fb and d.abe and d.alpha == (0, 2, 2, 2) and t["ztst"] == 2:
        return "Game_DrawHazeLayers or Game_DrawDepthTestSprite (the frame over itself at a depth)"
    if not d.tex and d.abe and d.alpha == (0, 1, 0, 1) and L["draws"] == 10 and d.zbuf[2] == 0:
        return "GsDraw_FillScreen(0, 0, 0, a, 0x44)"
    if d.path == 3 and d.tex:
        return "the menu loop's pass from TBP 0x%04X" % tbp
    if d.path == 3:
        return "the menu loop's untextured pass"
    if L["target"] == "other":
        return "a full-target draw into page %d" % d.frame[0]
    return "?"


def step_report(a, names):
    rows, scen_out = [], {}
    for n in names:
        folder = os.path.join(a.out, "scenarios", slug(n))
        done = os.path.join(folder, "captured.json")
        if not os.path.exists(done):
            continue
        capinfo = json.load(open(done))
        scen = {"name": n, "what": scenario(n)["what"], "forced": scenario(n)["forced"], "captures": []}
        for c in capinfo["captures"]:
            lst = os.path.join(folder, c["list"])
            per_scale = {}
            for scale in a.scales:
                p = os.path.join(folder, c["file"][:-6] + ".gpu_%s.txt" % scale)
                if os.path.exists(p):
                    per_scale[scale] = p
            base_scale = a.scales[0]
            draws, _ = parse_list(lst)
            own, other, reps = parse_times(per_scale.get(base_scale), len(draws))
            cap = Capture(lst, own, other)
            cap.analyse()
            scale_costs = {}
            for scale, p in per_scale.items():
                o2, x2, _ = parse_times(p, len(cap.draws))
                cap2 = (o2, x2)
                scale_costs[scale] = cap2
            frame_total = {}
            for scale, (o2, x2) in scale_costs.items():
                for k, (f0, f1) in enumerate(cap.frames):
                    frame_total.setdefault(scale, []).append(
                        sum(o2.get(i, 0) for i in range(f0, f1)) + sum(x2.get(i, 0) for i in range(f0, f1 + 1)))
            caprow = {"file": c["file"], "vblank": c["vblank"], "draws": len(cap.draws), "frames": cap.frames,
                      "complete": cap.complete, "small_targets": sorted(cap.small_targets),
                      "targets": {k: {"pages": [min(v["pages"]), max(v["pages"])], "count": len(v["pages"]),
                                      "how": sorted(v["how"])} for k, v in cap.targets().items()},
                      "repeats": reps, "frame_ms": frame_total, "layers": []}
            for k, L in enumerate(cap.layers):
                cls, why = classify(L, cap)
                L["class"], L["why"] = cls, why
                L["name"] = layer_name(L, cap)
                L["ms"] = {}
                for scale, (o2, x2) in scale_costs.items():
                    if "transfer" in L:
                        L["ms"][scale] = round(x2.get(L["first"], 0.0), 4)
                    else:
                        L["ms"][scale] = round(sum(o2.get(i, 0.0) for i in range(L["first"], L["last"] + 1)), 4)
                out = {k2: v for k2, v in L.items() if k2 not in ("writes_mask", "kills_mask", "reads_mask")}
                if "transfer" in out:
                    out["transfer"] = {k3: v3 for k3, v3 in out["transfer"].items()}
                caprow["layers"].append(out)
                rows.append(dict(scenario=n, capture=c["file"], **out))
            scen["captures"].append(caprow)
        scen_out[n] = scen
        lives = {}
        for rate in a.live_rates:
            p = os.path.join(folder, "live_%d.json" % rate)
            if os.path.exists(p):
                lives[rate] = json.load(open(p))
        scen["live"] = lives
    report = {"scales": a.scales, "scenarios": scen_out}
    json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=1, default=list)
    write_markdown(a, report, rows)
    print("report: %s" % os.path.join(a.out, "report.md"))


def family(name):
    """a layer's name without the values that change from frame to frame (a fill's colour, the brightness): the row
    of the summary, and those values"""
    m = re.match(r"GsDraw_FillScreen\((0x\w\w, 0x\w\w, 0x\w\w, 0x\w\w), (0x4[48]|ALPHA \d+)\)(.*)$", name)
    if m:
        return "GsDraw_FillScreen(r, g, b, a, %s)%s" % (m.group(2), m.group(3)), m.group(1)
    m = re.match(r"Game_ApplyBrightness \(the frame times (\d+) / 128\)", name)
    if m:
        return "Game_ApplyBrightness (the frame times the option / 128)", m.group(1)
    return name, ""


CLASS_NAMES = {"a": "(a) whole-buffer clear", "b": "(b) dead", "c": "(c) foldable into scanout",
               "d": "(d) must stay in memory", "e": "(e) other"}


def write_markdown(a, report, rows):
    base = a.scales[0]
    lines = ["# Full-screen layers: the tour's numbers", "",
             "Made by `tools/layer_inventory.py` (%s). GPU ms are the PC renderer's, replaying each capture at %s "
             "(`kf4-gsreplay --gpu-draws`, the median of %d repeats), a game frame's draws alone; the live columns "
             "are real-window runs at the owner's settings." % (time.strftime("%Y-%m-%d"), base, a.repeat), ""]
    # distinct layers
    distinct = collections.OrderedDict()
    for r in rows:
        if not r.get("complete"):
            continue
        fam, value = family(r["name"])
        key = (fam, r.get("signature", r["name"]))
        e = distinct.setdefault(key, {"name": fam, "class": collections.Counter(), "ms": [], "scenarios": set(),
                                      "example": r, "count": 0, "values": set()})
        if value:
            e["values"].add(value)
        e["class"][r["class"]] += 1
        if base in r.get("ms", {}):
            e["ms"].append(r["ms"][base])
        e["scenarios"].add(r["scenario"])
        e["count"] += 1
    lines += ["## Every layer", "",
              "| Layer | Values seen | Class | ms a game frame (median, max) | Captures | Scenarios | Writes | GS state |",
              "|---|---|---|---|---|---|---|---|"]
    for (name, sig), e in sorted(distinct.items(), key=lambda kv: -statistics.median(kv[1]["ms"] or [0])):
        ex = e["example"]
        cls = ", ".join("%s x%d" % (c, n) for c, n in e["class"].most_common())
        ms = "%.3f, %.3f" % (statistics.median(e["ms"]), max(e["ms"])) if e["ms"] else "-"
        gs = ex.get("state") or "; ".join("%s" % v for k, v in ex.get("gs", {}).items() if k in ("prim", "test",
                                                                                                  "alpha"))
        values = sorted(e["values"])
        lines.append("| %s | %s | %s | %s | %d | %s | %s | %s |" % (
            name, "; ".join(values[:6]) + (" ..." if len(values) > 6 else ""), cls, ms, e["count"],
            ", ".join(sorted(e["scenarios"]))[:120], ex.get("writes", "-"), gs))
    # per scenario: ms a game frame by class
    lines += ["", "## By scenario: ms a game frame by class (%s)" % base, "",
              "| Scenario | Frame (replay) | (a) clears | (b) dead | (c) foldable | (d) in memory | (e) other | "
              "Live 60: ms a picture | Live 30: ms a picture |", "|---|---|---|---|---|---|---|---|---|"]
    for n, sc in report["scenarios"].items():
        for c in sc["captures"]:
            frames = c["frame_ms"].get(base, [])
            judged = [k for k, ok in enumerate(c.get("complete", [])) if ok]
            if not frames or not judged:
                continue
            k0 = judged[0]
            f0 = frames[k0]
            by = collections.Counter()
            for L in c["layers"]:
                if L["frame"] == k0:
                    by[L["class"]] += L["ms"].get(base, 0.0)
            live = []
            for rate in (60, 30):
                lv = sc["live"].get(rate) or sc["live"].get(str(rate))
                if lv and lv.get("pictures"):
                    tot = sum(v[0] for v in lv["kinds"].values()) / 1e6 / lv["pictures"]
                    live.append("%.2f (%s frames)" % (tot, lv.get("game_frames")))
                else:
                    live.append("-")
            lines.append("| %s @%d | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %s | %s |" % (
                n, c["vblank"], f0, by["a"], by["b"], by["c"], by["d"], by["e"], live[0], live[1]))
    # what each change would save: per game frame (kf4-pc's frame) and per picture between (the pump's: it leaves
    # out what pcr_dead_writes.h finds dead already, the (b) layers)
    lines += ["", "## What each change would save (%s, ms)" % base, "",
              "Per game frame: the layers' replay ms in the capture's first judged frame. Per picture between: the same "
              "less the dead layers, which the pump already leaves out (`pcr_dead_writes.h`). Fast clears: the (a) "
              "layers' whole cost (a fast clear is close to free). Dead writes: the (b) layers, in the game frame "
              "only. Folding: the (c) layers, into the scanout that runs anyway. Blocked: (d) and (e).", "",
              "| Scenario | Frame | Fast clears (frame / between) | Dead writes (frame) | Folding (frame / between) | "
              "Blocked (d + e) | What blocks |", "|---|---|---|---|---|---|---|"]
    for n, sc in report["scenarios"].items():
        for c in sc["captures"]:
            frames = c["frame_ms"].get(base, [])
            judged = [k for k, ok in enumerate(c.get("complete", [])) if ok]
            if not frames or not judged:
                continue
            k0 = judged[0]
            by = collections.Counter()
            blockers = []
            for L in c["layers"]:
                if L["frame"] != k0:
                    continue
                ms = L["ms"].get(base, 0.0)
                by[L["class"]] += ms
                if L["class"] in "de" and ms >= 0.05:
                    blockers.append("%s %s (%.2f)" % (L["class"], family(L["name"])[0].split(" (")[0], ms))
            lines.append("| %s @%d | %.2f | %.2f / %.2f | %.2f | %.2f / %.2f | %.2f | %s |" % (
                n, c["vblank"], frames[k0], by["a"], by["a"], by["b"], by["c"], by["c"], by["d"] + by["e"],
                "; ".join(blockers)[:300]))
    # the targets and the GS memory they share with the frame
    lines += ["", "## Targets and the GS memory they share", "",
              "Every buffer drawn into, Z tested in, copied into, or read as a texture where something was drawn, over "
              "all captures: its pages (8 KB each) and which scenarios use it. The frame's buffers are FBP 0 and 140 "
              "(pages 0-139, 140-279), its Z buffer ZBP 280 (16-bit: pages 280-349).", "",
              "| Target | Pages | Overlaps | Used as | Scenarios |", "|---|---|---|---|---|"]
    frame_ranges = {"frame 0": (0, 139), "frame 1": (140, 279), "Z": (280, 349)}
    allt = collections.OrderedDict()
    for n, sc in report["scenarios"].items():
        for c in sc["captures"]:
            for k, v in c.get("targets", {}).items():
                e = allt.setdefault(k, {"lo": v["pages"][0], "hi": v["pages"][1], "how": set(), "sc": set()})
                e["lo"], e["hi"] = min(e["lo"], v["pages"][0]), max(e["hi"], v["pages"][1])
                e["how"] |= set(v["how"])
                e["sc"].add(n)
    for k, e in sorted(allt.items(), key=lambda kv: kv[1]["lo"]):
        over = [name for name, (lo, hi) in frame_ranges.items() if e["lo"] <= hi and e["hi"] >= lo]
        others = [k2 for k2, e2 in allt.items() if k2 != k and e2["lo"] <= e["hi"] and e2["hi"] >= e["lo"] and
                  not k2.startswith(("colour FBP 0 ", "colour FBP 140 "))]
        lines.append("| %s | %d-%d | %s | %s | %s |" % (k, e["lo"], e["hi"], ", ".join(over + others[:4]) or "-",
                                                        ", ".join(sorted(e["how"])),
                                                        ", ".join(sorted(e["sc"]))[:160]))
    # the live runs by shader state: a game frame from the 30 run (2 vblanks a game frame, the scanout and the
    # present twice), a picture between from the 60 run less the 30 run's (one picture between a game frame)
    lines += ["", "## Live, by shader state (real window, the owner's settings)", "",
              "A game frame: twice the 30 run's ms a picture (its two vblanks). A picture between: twice the 60 run's "
              "less the 30 run's (each game frame gets one), when both ran 30 game frames a second. The scanout "
              "(\"pc picture\") and the present are counted in both, every vblank.", ""]
    for n, sc in report["scenarios"].items():
        lv = {int(k): v for k, v in sc["live"].items()}
        if 30 not in lv or not lv[30].get("pictures"):
            continue
        l30, l60 = lv[30], lv.get(60)
        lines += ["### %s" % n, "", "Game frames in the %d s window: %s at 30, %s at 60." % (
            l30["seconds"], l30.get("game_frames"), l60.get("game_frames") if l60 else "-"), "",
                  "| ms | a game frame (30) | a picture between (60 - 30) | intervals a game frame |", "|---|---|---|---|"]
        for table, title in (("kinds", "kind"), ("shaders", "shader state")):
            names = set(l30[table]) | set((l60 or {}).get(table, {}))
            items = []
            for name in names:
                a30 = l30[table].get(name, (0, 0))
                p30 = a30[0] / 1e6 / l30["pictures"]
                if l60 and l60.get("pictures"):
                    a60 = l60[table].get(name, (0, 0))
                    p60 = a60[0] / 1e6 / l60["pictures"]
                    between = 2 * (p60 - p30)
                else:
                    between = None
                frames = max(1, l30.get("game_frames") or l30["pictures"] // 2)
                items.append((2 * p30, between, a30[1] / frames, name))
            items.sort(reverse=True, key=lambda x: x[0])
            for g, b, iv, name in items[:16 if table == "shaders" else 20]:
                if g < 0.01 and (b is None or abs(b) < 0.01):
                    continue
                lines.append("| %s: %s | %.3f | %s | %.1f |" % (title, name, g, "-" if b is None else "%.3f" % b, iv))
        lines.append("")
    # the layers of each scenario
    lines += ["", "## Each capture's layers", ""]
    for n, sc in report["scenarios"].items():
        lines += ["### %s%s" % (n, " (forced)" if sc["forced"] else ""), "", sc["what"], ""]
        for c in sc["captures"]:
            lines.append("- `%s`: %d draws, game frames %s, replay ms a frame %s" % (
                c["file"], c["draws"], c["frames"], ", ".join("%s: %s" % (s, ", ".join("%.2f" % x for x in v))
                                                             for s, v in c["frame_ms"].items())))
            for L in c["layers"]:
                rd = L.get("readers", {})
                rtext = rd if isinstance(rd, list) else "; ".join("%s: %s" % (k, ", ".join(v[:3]) or "none")
                                                                 for k, v in rd.items())
                if not L.get("complete"):
                    lines.append("  - draws %d-%d (frame %d) %s: its frame is shown after the capture's end, not "
                                 "judged" % (L["first"], L["last"], L["frame"], L["name"]))
                    continue
                lines.append("  - draws %d-%d (frame %d) **%s**: %s; %s; writes %s; ms %s; readers: %s%s" % (
                    L["first"], L["last"], L["frame"], L["name"], CLASS_NAMES.get(L["class"], L["class"]), L["why"],
                    L.get("writes"), ", ".join("%s %.3f" % (s, v) for s, v in L.get("ms", {}).items()), rtext,
                    "; end: %s" % L["end"] if L.get("end") else ""))
        lines.append("")
    open(os.path.join(a.out, "report.md"), "w").write("\n".join(lines) + "\n")


# ---- Main -------------------------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--out", required=True, help="the output folder (outside the repository)")
    p.add_argument("--disc", default=os.environ.get("KF4_DISC"))
    p.add_argument("--exe")
    p.add_argument("--gsreplay")
    p.add_argument("--scenarios", help="names, comma separated; list; all (the default)")
    p.add_argument("--maps", default="", help="all, or map numbers: a scenario for each (a quick load and a walk)")
    p.add_argument("--steps", default="capture,time,report", help="capture,list,time,live,report (list: the "
                   "captures' lists made again)")
    p.add_argument("--scales", default="3x2", help="replay scales, the first the main one (3x2: the owner's 2x with "
                   "native widescreen; 2x1: 1x; 6x4: 4x)")
    p.add_argument("--scale-scenarios", default="", help="the scenarios timed at every scale (the others at the "
                   "first only); default all")
    p.add_argument("--repeat", type=int, default=12, help="replays of each capture (the first two not counted)")
    p.add_argument("--jobs", type=int, default=3, help="headless captures at once")
    p.add_argument("--live-rates", default="60,30")
    p.add_argument("--live-seconds", type=int, default=4)
    p.add_argument("--live-newgame", action="store_true", help="live runs from a New Game every time (by default a "
                   "scene with no input of its own starts from the base quick save: 25 s sooner)")
    p.add_argument("--force", action="store_true", help="do steps again whose outputs exist")
    a = p.parse_args()
    if a.scenarios == "list":
        for n, sc in SCENARIOS.items():
            print("%-22s %s%s" % (n, sc["what"], " (forced)" if sc["forced"] else ""))
        print("%-22s %s" % ("map NNN", "(--maps) a map's start: a quick load, a walk and a turn"))
        return
    if not a.disc:
        sys.exit("layer_inventory: pass --disc or set KF4_DISC")
    a.exe = a.exe or map_tour.find_exe("kf4")
    a.gsreplay = a.gsreplay or map_tour.find_exe("kf4-gsreplay")
    a.out = os.path.abspath(a.out)
    if a.out == ROOT or a.out.startswith(ROOT + os.sep):
        sys.exit("layer_inventory: --out must be outside the repository (captures and pictures are the game's)")
    os.makedirs(a.out, exist_ok=True)
    a.scales = [s.strip() for s in a.scales.split(",") if s.strip()]
    a.scale_scenarios = [s.strip() for s in a.scale_scenarios.split(",") if s.strip()]
    a.live_rates = [int(r) for r in a.live_rates.split(",") if r.strip()]
    a.disc_files = map_tour.Disc(a.disc)
    maps = a.disc_files.maps() if a.maps == "all" else [int(m) for m in a.maps.split(",") if m.strip()]
    names = scenario_names(a.scenarios, maps)
    for n in names:
        scenario(n)
    steps = set(a.steps.split(","))
    a.base_path = None
    if (any(n.startswith("map ") for n in names) and "capture" in steps) or "live" in steps:
        base_path = os.path.join(a.out, "base.kf4quick")
        if not os.path.exists(base_path):
            map_tour.make_base(a.exe, a.disc, a.out, None)
        a.base = map_tour.read_base(base_path)
        a.base_path = base_path
    if "capture" in steps:
        step_capture(a, names)
    if "list" in steps:
        step_list(a, names)
    if "time" in steps:
        step_time(a, names)
    if "live" in steps:
        step_live(a, names)
    if "report" in steps:
        step_report(a, names)


if __name__ == "__main__":
    main()
