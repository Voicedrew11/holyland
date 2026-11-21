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
`--keep-frames` keeps the pictures (deleted after scoring otherwise: about 5 MB each at 2x); `--approve` writes each
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
| `threshold`, `radius`, `max_score` | 12, 1, 25 | the robust measure (below): levels, pixels, flip pixels per 100,000 allowed |
| `fine_threshold`, `fine_max` | 24, 40 | the fine measure: levels, flip pixels per 100,000 allowed |
| `walk_threshold`, `walk_radius`, `walk_max` | 24, 2, 25 | the walk's measure |
| `region` | the whole picture | `[x0, y0, x1, y1]`, fractions of the picture, the part measured |
| `ignore` | `[]` | more `[x0, y0, x1, y1]` boxes left out (the HUD's corner always is) |
| `scale`, `frame_rate` | 2, 240 | `--native-scale`, `--frame-rate` |
| `args`, `env` | | more options and environment for the game (an option a build does not know is dropped) |
| `teleport_first` | | a map to visit before the spot (`--teleport`), for state that lasts across maps |
| `settle` | 90 | vblanks from a view's load to its picture (the fade in takes about 70) |
| `quick` | | a quick save file to load instead of the numbers (local spots only: never commit a save) |
| `found`, `approved` | | written by the tools: the score when the spot was added, and with `--approve` |

The game is played with the owner's picture settings (`OWNER` in the script: native draw on, widescreen with
native widescreen, FOV 64, FXAA, anisotropy 16, texture detail full, layer Z tolerant; for builds from before FXAA,
MSAA 4 and cut-out AA as played then), at 2x and `--frame-rate 240`.

## How a spot is played

The test needs no saves of its own: it first makes a quick save of a new game with the build under test
(`tests/newgame.pad`, `--quick-save-at 1900`; cached in `OUT/base`), then writes from it one quick save per view
with the map, the position and the rotation changed and marked "made up" (as `map_tour.py` does: on loading, the
game puts the player on the floor under the position and loads the neighbouring map, `game/quicksave.c`). All the
views of a spot are loaded one after another in one headless run (`--quick-load A,V:B,...`) and pictured 90 vblanks
after each load (`--dump-frames`). Older builds (back to October 2026) load these files too, so any build can be
tested; options they do not know are dropped (`--saves` becomes `--memcard`).

A new game's save holds a new game's progress: doors, broken walls, creatures and items are as at the start. For
the eight spots below the picture made from the numbers matches the owner's own quick save there (outside the HUD:
0.00 to 0.01% of pixels differ by more than 16 levels; `zfight_add_spot.py` checks this for every new spot and
warns above 1%). The player's level shows only in the HUD, which is left out.

## The motions

- `turn`: the heading turned `step` degrees a view (nine views, 0.16 degrees in all, about 3 pixels at 2x).
- `step`: the player moved forward and back `step_units` a view; `side`: sideways.
- `walk`: the game's own walk: from one load, short presses of up and down (the walk speeds up from about 1 unit a
  frame), every picture at 240: each vblank's and its three pictures between (`frame_V_250.png` ...), so the
  pictures between game frames, where the camera and models are moved on the GPU, are measured too. The walk is
  played twice from the same load.

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

Each counts flip pixels per 100,000 pixels of the picture (2x widescreen: 1920x896) against its limit; the motion's
score is the larger, in percent of its limit, and the spot passes at 100% or less.

What changes with time rather than with the view (the HUD's orb, torches and their light, water, creatures) would
flip too. Whatever differs (by more than 12 levels, grown by 2 pixels) between a sweep's middle view and its two
repeats, in any of the three sweeps, is "animated" and not counted; for the walk, the two plays are compared
picture by picture. The HUD's corner is never counted. A sweep whose middle view and its repeats differ a lot (more
than 4 levels on average, or more than a quarter of the picture animated: a flash of poison, a creature in the
face) is an ERROR, not a pass.

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

## What it catches (2026-11-21)

The eight spots in `tests/zspots/` (from the owner's quick saves of 2026-11-13 to 11-15) against builds before and
after the November z-fighting fixes: each spot's score in percent of its limit and the motion that scored it (bold:
FAIL). The cave's robust limit is 6 (`max_score` in its JSON; good builds score 2.1 to 2.3 there), every other
limit the default.

| spot | 646df6d0 (#143 draft, bad) | 23d96016 (#143 draft) | main 482ec65b | main 57ea5912 (#160) | f7b9a2b9 (good) | c1a45c2c (good) | aa6c2efa (#143, good) | main 5834c15e (today) |
|---|---|---|---|---|---|---|---|---|
| chest-far-1114 | **947** walk | 44 turn | 43 turn | 48 turn | 47 turn | 47 turn | 47 turn | 2 turn |
| ground-rect-1115 | **792** side | 10 turn | 10 turn | 10 turn | 10 turn | 10 turn | 10 turn | 0 step |
| nearwall-1113 | 20 side | 14 turn | 20 side | 20 side | 16 side | 16 side | 16 side | 22 turn |
| reflect-1114 | 5 side | **1876** side | 5 side | 6 side | 5 side | 6 side | 6 side | 3 turn |
| sign-1114 | **489** side | 26 step | 18 turn | 14 turn | 9 turn | 9 turn | 9 turn | 8 step |
| zfight-bridge-1113 | 53 turn | 43 turn | 69 turn | 35 turn | 36 turn | 36 turn | 36 turn | 23 turn |
| zfight-cave-1113 | **4738** turn | 35 turn | **560** turn | 38 turn | 38 turn | 38 turn | 38 turn | **144** turn |
| zfight-rock-1113 | **268** side | 76 side | **144** side | **130** side | 76 side | 76 side | 76 side | 81 side |

- 646df6d0, #143's draft with "new z-fights everywhere" (owner, 11-13): whole triangles of the cave's overlay, the
  sign, the far chest's surroundings, the floor overlay at map 00's door and the rock's top flip; FAIL at five spots
  by 2.7 to 47 times the limit.
- 23d96016, #143's draft before the ripple corner order (fixed in 8c1e4a8a): the water's reflection hatched; FAIL
  at the reflection spot by 19 times. Its far chest and sign flicker (the owner's chest-far-1114 and sign-1114) do
  not show headless, here or with 4x pictures: those two spots pass in every build.
- main 482ec65b and 57ea5912 (#160): the rock's top face stripes (fine measure 52-58 against 30 in good builds) and
  in 482ec65b the cave (robust 33.6 against 2.3); FAIL.
- f7b9a2b9, c1a45c2c, aa6c2efa (the builds the owner called fixed): every spot passes, the highest at 76% (the rock's
  ridge: aliased edge speckle, the same in every build).
- main 5834c15e (the scene renderer): the cave FAILs at 144%: a checkered patch on the torch-lit tunnel wall that
  older builds do not draw; the rock is at 81% (its top-face stripes are back faintly: robust 20.2 against 9.8).

Which motion shows the fights: the sweeps (turn, step, side) catch every one above; side is the strongest for the
rock's stripes and the floor overlay, turn for the cave. The walk (with the pictures between) catches the same ones
(646df6d0's far chest most strongly, 948%) and none the sweeps miss; it is the only motion that measures the
pictures between game frames. The bridge spot shows nothing in any build (the highest 69%, main 482ec65b), and the
owner's dark rectangle at map 00's door (ground-rect-1115, after walking into map 63h) is a still picture, not a
flicker, and did not show after a teleport to map 99 either. nearwall-1113 is a negative control without the walk
(the triangle at the screen's edge there is the game's own clipping).

The whole suite takes about 4 minutes with `--jobs 2` (2x, 240), the first run of a build 20 s more for its base
save.

## Limits of the test

- It sees flicker, not a picture that is wrong but still (a missing overlay, a dark rectangle that stays put).
- Headless pictures are the frame at each vblank and, at 240, the pictures between; flicker seen only at an unlocked
  rate with the mouse turning (the far chest and sign of 11-14) was not reproduced.
- The views are a new game's progress at the spot's place (checked against the quick save for each spot: 0.00 to
  0.01% of pixels differ, outside the HUD). Where progress changes what is seen, `zfight_add_spot.py` warns.
- The limits come from the builds above, at 2x with FXAA (MSAA 4 before FXAA existed); another scale or
  anti-aliasing moves the edge speckle a little (the rock: 76-81% in good builds).
