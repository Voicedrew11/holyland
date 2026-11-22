# Z-fighting test (`tools/zfight_test.py`)

Surfaces that fight over depth (a decal or overlay stitching through its wall or floor, rock edges, bridge planks,
far chests and signs flickering) came back with several renderer changes in November 2026, each time found by the
owner in play. This test plays the places where they were seen, headless and silent, and fails a build that shows
flicker there.

```sh
KF4_DISC=<image> python3 tools/zfight_test.py                       # every spot in tests/zspots, out/ build
python3 tools/zfight_test.py --spot zfight-cave-1113 --exe <build>/kf4 --out /tmp/zf   # one spot, any build
python3 tools/zfight_add_spot.py NAME --about "what fights"         # a new spot from your newest quick save (F2)
```

It prints a line a spot (PASS / FAIL / ERROR, the score, the score of each motion and the evidence picture), writes
`OUT/report.md` and `report.json`, and exits 1 when a spot does not pass. `--jobs 2` runs two spots at once;
`--keep-frames` keeps the pictures (deleted after scoring otherwise: about 10 MB each at 3x); `--keep-maps` keeps
each measure's flips (`maps_MOTION.npz`); `--motion turn,side` runs only those motions (quicker); `--settings`,
`--screen-height`: the picture settings (below); `--approve` writes each
spot's score into its JSON as `approved` (a record of what a good build scored; it changes no limit). Options after
`--` go to every run of the game.

## A spot

A spot is a small JSON file in `tests/zspots/` (or `--spots DIR`, or `KF4_ZSPOTS`): where the player stands, and
nothing of the game's data, so it is committed. Only `map`, `position` and `heading` are needed:

```json
{
  "about": "far chest and sign flicker (owner, 2026-11-14)",
  "map": 0,
  "position": [-384.899, -0.0, -5714.246],
  "heading": -0.05233,
  "pitch": -0.02444
}
```

| field | default | what |
|---|---|---|
| `map`, `position`, `heading`, `pitch` | (pitch 0) | the map number, the player's x, y, z (the game's units; y grows downward) and rotation in radians, as a quick save holds them (`PlayerState`) |
| `motion` | `"all"` | `turn`, `step`, `side`, `walk`, a list of them, or `all` (below) |
| `views` | 9 | views in each sweep |
| `step` | 0.02 | `turn`: degrees between views (0.4 pixels at 2x) |
| `axis` | `heading` | `turn` turns the heading, or the `pitch` |
| `step_units` | 0.5 | `step` and `side`: the game's units between views |
| `walk` | `[["UP",4,4],["DOWN",4,4],["UP",4,4],["DOWN",4,6]]` | `walk`: pad presses, each `[button, vblanks held, vblanks of rest after]` |
| `threshold`, `radius`, `max_score` | 12, 1, 20 | the robust measure (below): levels, pixels, flip pixels per 100,000 of the window allowed |
| `fine_threshold`, `fine_max` | 24, 40 | the fine measure: levels, flips allowed |
| `walk_threshold`, `walk_radius`, `walk_max` | 24, 2, 25 | the walk's measure |
| `cluster`, `window` | 5, 16 | a flip counts with at least this many in its 5x5; the score's window is 1/`window` of the width |
| `time_mask` | true | leave out what moves by itself in a game frame (flames, water, idle animations) |
| `region` | the whole picture | `[x0, y0, x1, y1]`, fractions of the picture, the part measured |
| `ignore` | `[]` | more `[x0, y0, x1, y1]` boxes left out (the HUD's corner always is) |
| `scale`, `frame_rate` | auto, 240 | `--native-scale` (auto: what the settings' auto gives on `--screen-height`), `--frame-rate` |
| `args`, `env` | | more options and environment for the game (an option a build does not know is dropped) |
| `teleport_first` | | a map to visit before the spot (`--teleport`), for state that lasts across maps |
| `settle` | 90 | vblanks from a view's load to its picture (the fade in takes about 70) |
| `quick` | | a quick save file to load instead of the numbers (local spots only: never commit a save) |
| `found`, `approved` | | written by the tools: the score when the spot was added, and with `--approve` |

## The picture settings

The runs use the player's own settings: `--settings FILE` (default the player's `settings.json` in the data folder,
`%APPDATA%\holyland` on Windows, when there is one; `--settings default` or no file: built-in defaults, the owner's
of 2026-11-21). The file is only read: each run gets a copy in its own data folder (`KF4_DATA_DIR`) with
`KF4_READ_SETTINGS=1`, so it reads it headless as a window would. In the copy the things that draw over the picture
are off (the automap's minimap, the extended stats panels, the hands' cards, the frame-rate counter). Builds from
before `KF4_READ_SETTINGS` (2026-11-15) get the same settings as options (widescreen, FOV, anti-aliasing,
anisotropy, texture detail, layer Z, native draw, sky, draw distance; MSAA 4 and cut-out AA for builds before FXAA,
as played then).

Two things are always options, winning over the file: `--frame-rate` (the spot's, 240: headless pacing is the game's
own vblanks, so the pictures between are the same in every run) and `--native-scale`. A headless run has no screen,
where the settings' `"auto"` gives 1, so the test works out what auto gives on the player's screen:
`--screen-height` (default 1440, the owner's 2560x1440: the picture's height over 448, rounded, so 3; 1080 gives 2,
2160 gives 4). A spot's `scale` overrides it. The scale matters: the cave's and the rock's fights score much higher
at 3 and 4 than at 2.

## How a spot is played

The test needs no saves of its own: it first makes a quick save of a new game with the build under test
(`tests/newgame.pad`, `--quick-save-at 1900`; cached in `OUT/base`), then writes from it one quick save per view
with the map, the position and the rotation changed and marked "made up" (as `map_tour.py` does: on loading, the
game puts the player on the floor under the position and loads the neighbouring map, `game/quicksave.c`).

Each view is a run of its own: the game started afresh, the view's save loaded on the title screen, the picture
`settle` vblanks later. Every run loads at the same vblank, so the game's own time is the same in all of them: a
torch's flicker, the water, the creatures' idle animations and the HUD are exactly as in every other view, and only
the view differs. (Loaded one after another in one run, the views were pictured at different times, and the torch
light alone moved the cave's wall by up to 37 levels between them.) The spot itself is played twice, as a check: the
two pictures are the same, and anything that differs is left out. About 25 runs a spot, 1 to 2 seconds each.

Older builds (back to October 2026) load these files too, so any build can be tested; options they do not know are
dropped (`--saves` becomes `--memcard`).

A new game's save holds a new game's progress: doors, broken walls, creatures and items are as at the start. For
the spots below the picture made from the numbers matches the owner's own quick save there (outside the HUD: 0.00
to 0.19% of pixels differ by more than 16 levels; `zfight_add_spot.py` checks this for every new spot and warns
above 1%). The sick lady's daughter and the dog (npc-models-1121) come into view some seconds after a load, in a new
game's save as in the owner's: that spot has `"settle": 600`.

## The motions

- `turn`: the heading turned `step` degrees a view (nine views, 0.16 degrees in all, about 3 pixels at 2x).
- `step`: the player moved forward and back `step_units` a view; `side`: sideways.
- `walk`: the game's own walk, by pad presses (`walk`: `[button, vblanks held, vblanks of rest after]`): by default
  short presses of up and down (the walk speeds up from about 1 unit a frame); a spot can walk further, e.g.
  chest-walkup-1121 walks toward its chest for 40 vblanks. Every picture at 240: each vblank's and its three pictures
  between (`frame_V_250.png` ...), so the pictures between game frames, where the camera and models are moved on the
  GPU, are measured too. The walk is played twice, in two runs.

Each sweep (turn, step, side) is nine views with the middle view loaded once more before and after them. Each
motion is scored on its own and the spot's score is the worst motion's.

## The measure: flips under a tiny move

When the view moves by a fraction of a pixel, a pixel's colour changes smoothly: its colour in a view lies between
(or very near) its colours in the view before and the view after. Where two surfaces lie in the same plane and the
depth test cannot tell them apart, which one wins changes from one tiny move to the next, so the pixel jumps away
from both its neighbours and back: it is above both, or below both, by more than a threshold in some colour
channel. Such a pixel is a flip. Three measures count them:

- robust (sweeps): the neighbours' colours are taken from the 3x3 square around the pixel (`radius` 1), threshold
  12 levels. An aliased edge or a thin pole crossing a pixel under a sub-pixel move flips it once, but its colour
  is right beside it in the neighbours, so it does not count; a fighting surface's colour is nowhere near. 12
  levels because the fights are often between two dim surfaces (the cave's checkered patch is 12-16 levels).
- fine (sweeps): the neighbours' own pixel, threshold 24. Stripes one pixel wide (a rock's overlay fighting its
  face in thin diagonal stripes) are lost in the robust measure's square; this keeps them, and the aliased edges
  too, hence its higher limit.
- walk: the neighbours' colours within 2 pixels (the walk moves the picture faster), threshold 24.

Only flips with at least `cluster` (5) flips in the 5x5 square around them count: a fighting patch or its stripes,
not the odd pixel of an aliased edge. The score is local: the flips in the densest window, a square 1/16 of the
picture's width (160 pixels at 3x), per 100,000 pixels of that window, so one small or distant fighting object (a
far chest, a bridge's planks) fails like a whole wall. (Counted over the whole picture, as before 2026-11-21, the
broken bridge's planks scored 23% of the limit on the preview build.) Limits: robust 20 (`max_score`), fine 40
(`fine_max`), walk 25 (`walk_max`): about 5, 10 and 6 pixels of a 160-pixel window. The motion's score is the larger
of its measures in percent of its limit, and the spot passes at 100% or less.

What changes with time rather than with the view (the HUD's orb, torches and their light, water, creatures) cannot
flip: every view is pictured at the same moment of the game (each its own run, above). A check: the spot is played
twice, and what differs between the two (by more than 12 levels, grown by 2 pixels) is not counted; it is normally
nothing. With `time_mask` (on by default) the repeat is also pictured a game frame later, and what moves by itself
in that frame (a torch's flame, which otherwise flips under a sub-pixel move as its sprite snaps, water) is not
counted either; a spot about a moving thing sets it false. For the walk, the two plays are compared picture by
picture. The HUD's corner is never counted. A spot whose two plays differ a lot (more than 4 levels on average, or
more than a quarter of the picture: a flash of poison, a creature in the face) is an ERROR, not a pass.

No approved pictures: changes to lighting, fog, filtering or anti-aliasing move colours smoothly and do not flip,
so the test fails only on new flicker. `evidence.png` shows the worst 256x256 part of the worst motion's middle
picture (zoomed), the size of each pixel's largest jump there (red; left-out pixels blue), and the whole picture
with every flip marked red.

## Adding a spot

In the game: stand where it flickers, face it, press F2. Then `tools/zfight_add_spot.py NAME --about "..."` (on
the owner's PC: `C:\code\kf4-zspot-add.bat NAME "what fights"`). It reads the newest quick save in the data folder
(`quicksaves/`, or `--quick FILE`) for the map, position, heading and pitch only (the save is not copied into the
spots folder and nothing in the data folder is written), writes `NAME.json`, checks that the spot made from the
numbers shows the same picture as the quick save, runs the test on it once and writes the score into the JSON as
`found`. A spot added with a build that shows the fight should fail; if it passes, the test does not see that fight
(try a `region` around it, `"motion": "walk"`, a larger `step`, or a lower `threshold`).

## What it catches (2026-11-21, evening: local scoring, the player's settings, one run a view)

The preview build 2fdf828e (scale 3 from the owner's settings at 2560x1440): each spot's score in percent of its
limit, the motion that scored it, and where the worst window is (`evidence.png`).

| spot | preview 2fdf828e | worst motion | where | owner |
|---|---|---|---|---|
| bridge-broken-1121 | **1992** | turn | the broken bridge's lower planks (diagonal stripes) and their ends | fights |
| zfight-bridge-1113 | **2364** | turn | the same bridge, further off | (11-13 spot) |
| door-braces-1121 | **1640** | step (walk 1000) | the door's braces | fights |
| ruins-corner-1121 | **2539** | step | diagonal hatching where the ruins' wall meets the ground | fights |
| npc-models-1121 | **1153** | side | a vertical line by the window shutter on the house wall, not the daughter or the dog | fights on the models |
| door-frames-1121 | **860** | turn | vertical lines on the wooden frames and planks | fights |
| zfight-rock-1113 | **957** | side | the rock's top face and ridge | (11-13 spot) |
| zfight-cave-1113 | **235** | step | the lit back wall right of the torch | fights (off-centre view) |
| wall-seam-1121 | **235** | side | the helmet on the rampart, not the seam (a `region` around the seam's side) | fights at the seam |
| chest-walkup-1121 | **225** | turn (walk 125) | the torch brazier by the door, not demonstrably the chest | fights on the chest |
| sign-1114 | **332** | step | where the rock meets the low wall left of the door | (11-14 spot) |
| temple-wall-1121 | **117** | step | the low wall's base by the steps | fights |
| nearwall-1113 | **469** | side | a vertical dashed line at a seam of the wall | (control) |
| chest-far-1114 | 78 | turn | the torch braziers by the door | (11-14 spot) |
| ground-rect-1115 | 0 | | | (11-15 spot) |
| reflect-1114 | 0 | | | (11-14 spot) |

The whole suite takes about 12 minutes there (`--jobs 2`; 16 spots, about 25 runs each at 3x; `--motion turn,side`
about half).

Checks on the measure:
- Lighting cannot count: every view is pictured at the same moment of the game. The spot itself played twice gives
  identical pictures (byte for byte at the first load); a sweep with no turn at all (step 0) scores 0 at the cave,
  the bridge and the broken bridge, where the same views loaded one after another in one run had differed by up
  to 37 levels on the torch-lit wall. The cave's fight is real (the owner saw it from this angle): it flips with
  the view and not with time.
- The torch's flame flips under a sub-pixel move as its sprite snaps; the time mask (the repeat a game frame later)
  leaves it out (reflect-1114: 1543% before, 0 after). The brazier under the flame still flips a little (chest-far,
  chest-walkup).
- Scoring over the whole picture, the broken bridge scored 23% on this build; the densest window scores it 1992%.

The builds from before FXAA (2026-11-16) are not good references for this measure at 3x with these settings. 646df6d0
fails every spot by 10 to 3000 times (whole triangles flip), as before; but f7b9a2b9, which the owner called fixed
at the spots of 11-13 and 11-14, fails most spots too: its textures and thin poles shimmer under a sub-pixel move
(the house's planks at npc-models-1121, the cave's torch stand), the screen's right edge strip flips (door-braces),
and some flips look like real fights the 2x whole-picture measure had missed (a striped rock ridge at
ground-rect-1115). The limits were therefore set on the preview build: every spot the owner reported fails, and the
spots with nothing reported pass (reflect, ground-rect) or stay under the limit (chest-far, 78%).

## Limits of the test

- It sees flicker, not a picture that is wrong but still (a missing overlay, a dark rectangle that stays put).
- Headless pictures are the frame at each vblank and, at 240, the pictures between; flicker seen only at an unlocked
  rate with the mouse turning (the far chest and sign of 11-14) was not reproduced.
- The views are a new game's progress at the spot's place (checked against the quick save for each spot: 0.00 to
  0.01% of pixels differ, outside the HUD). Where progress changes what is seen, `zfight_add_spot.py` warns.
- The limits come from the preview build at 3x with the owner's settings (FXAA); builds before FXAA shimmer more
  and fail most spots (above). Where a spot's worst window is not on the reported fight (npc-models, wall-seam,
  chest-walkup), the spot fails for something else: look at its evidence.
- Fights on animated models: the creatures are pictured at one pose; the daughter's and the dog's fights did not
  show at that pose.
