# Handoff: lift the hot VU1 programs to C++ (issue #1, Stage A of #9)

Written 2026-10-09. Branch `vu1-census` (pushed, no PR yet). Read `AGENTS.md`,
`docs/contributing.md` and the "Performance" and "VU1 census" notes in
`docs/NOTES.md` first.

## Goal

Make the first 3D area run at ~30 fps (now ~12.2 flips/s, Ryzen 7 5700X dev
build) by replacing the VU1 interpreter for the two hot entry points with
hand-lifted C++ that is **bit-exact including cycle counts**. The lifted code
is also the specification of what the game sends to the GPU, which Stage C
(structured draws into a GPU backend, issue #9) needs later, so write it
readable, not as generated soup.

## What is established

- Baseline: dev build averages 12.2 flips/s over ticks 3000-3599 of the
  benchmark in `docs/contributing.md` (the issue's benchmark, `PS2X_STATS=1`).
  Run-to-run noise is about +-3%, so only trust changes bigger than that.
- The `GameThread` is ~85% VU1 interpreter, profile flat. About 35-40% of
  samples are FMAC flag code, the rest per-cycle bookkeeping (`run`,
  `execUpper`, `execLower`, `calculatePairReadyCycle`, decoded-pair lookup).
- Micro-optimising the interpreter does not get there. An exact SSE2
  classification fast path for `normalizeFmacResult` (68% of FMAC ops hit it,
  then zeros added) moved fps by 1-3%, within noise. It was dropped, not
  committed.
- Flag liveness (option 1 in #1) is blocked: every FMAC op ORs sticky bits
  into `status`, and the next microprogram may read them, so skipping the
  work breaks the verifier's flag comparison unless a cross-program analysis
  proves otherwise. A lifted program may compute flags lazily but must leave
  identical MAC/status/clip at exit.
- **Census** (`PS2X_VU1_CENSUS=<file>`, patch 0015): run to tick 3300 used
  one 16 KB microcode image (hash `4522ee03bb2e88f9`) and four entry PCs:
  `0x0000` 85% of VU1 cycles (41k MSCAL + 391k MSCNT resumes), `0x1400` 13%,
  `0x3800` 0.8%, `0x13b0` 0.3%. The image is at
  `runs/census/vu1.4522ee03bb2e88f9.bin` (untracked game data; regenerate
  with the command below). It was only observed up to tick 3300; check other
  scenes (movie, other areas) for further hashes before assuming one image.
- Programs run in E-bit-terminated segments. `execute()` calls
  `resetScheduler()` (clears pipelines and `m_ready`); every segment ends with
  `flushPipelines()`. `resume()` (MSCNT) does not reset but starts from the
  flushed state. So **block entry pipeline state is quiescent**, which is what
  makes static cycle counts per block possible.

## Progress (2026-10-10)

Steps 1 and 2 below are done; step 3 has not started. Nothing is committed.

- `scripts/maintainer/vu1dis.py` (untracked) disassembles the image and
  builds the CFG: `vu1dis.py <image.bin>` for a full listing,
  `--entry 0x0000 [--cfg]` for one entry's blocks. It follows MSCNT resume
  points (the pair after an E bit's delay slot) and splits blocks at branch
  targets. A pair that is both a branch's delay slot and another branch's
  target stays in both blocks, so the lift has to emit it once per path
  (`0x2298` in `0x1400`, `0x08d0` in `0x0000`). No unknown opcodes in the
  image.
- Listings and Haiku-written annotations are in `runs/vu1-lift/`
  (`entry_*.txt`, `annotated_0x0000.md`, `annotated_0x1400.md`). The
  annotations are a static reading; the "purpose" parts are guesses, not spec.
- Entry `0x0000`, 12 blocks: `0x0000`-`0x02f8` is setup that runs only on
  MSCAL (matrix products, light setup, an XGKICK at `0x00b8`). MSCNT resumes at
  `0x08f8`, which is `B 0x0300`: the per-packet prologue. `0x0400`-`0x04e8` is
  the per-vertex loop, software-pipelined one vertex ahead. `0x04f0` is the
  loop exit, with an XGKICK at `0x0650`. `0x06f8`-`0x08d0` is a light loop. The
  E bit is at `0x08e8`.
- Entry `0x0000` items to check against the interpreter before relying on
  them:
  - The `DIV Q, vf00w, vf31w` at `0x0358` reads `vf31.w` one pair before the
    `MADDw` at `0x0360` writes it, so vertex 0's Q may be stale.
  - If N <= 2, the vertex-loop counter (`vi11 -= 2`, then `IBNE`) may wrap.
  - The XGKICK packet at `vi05+160` overlaps output record 0.
- Entry `0x1400`, 45 blocks, resumes at `0x2550`. Its only flag readers are
  `FCAND vi01,0x3e0` after `FCSET`/`CLIP` (no FMAND/FSAND/FCGET). XGKICK
  points are at `0x1518`, `0x1f98`, `0x22f0` and `0x2530`. It has no RSQRT or
  EFU ops. `DIV` is at `0x1b78`, `0x1cd0`, `0x1e40` and `0x1f20`.
- Entry `0x0000`'s flag reads also appear to be only `FCAND` on clip flags. If
  that holds, lazy flags reduce to "MAC/status exact at exit, clip exact at
  each FCAND".
- Still open: verify that pipeline state is quiescent at every *block* entry
  (it is only established at segment entry). Stalls inside a segment carry
  across branches, so per-block static counts may need per-edge counts, or a
  small entry-state key. Also: other scenes' hashes, and the maintainer
  decision on committing lifted code.

## Next step

1. Disassemble the image. Use the image above and the lower/upper decode in
   `ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp` (`decodeUpperUsage`,
   `decodeLowerUsage`) as the reference for what each pair does and its
   latencies; PCSX2 and the VU manual are the hardware reference.
2. Build the control-flow graph for entries `0x0000` and `0x1400`: basic
   blocks, branch delay slots (`branchPending`/`branchDelay`), E-bit and
   D/T-bit ends, XGKICK points.
3. Lift one entry to C++ (start with `0x0000`): per-block static stall and
   cycle counts relative to block entry, VF/VI/ACC/Q/P semantics, flags
   computed lazily but exact at block exits and wherever a flag reader
   (FMAND/FMEQ/FMOR/FSAND/FSEQ/FSOR/FSSET/clip ops) needs them, and PATH1
   XGKICK output identical to the interpreter's.
4. Hook it into `VU1Interpreter::execute/resume`: dispatch on (code hash,
   entry PC), fall back to the interpreter for anything else, including
   an unknown hash. Keep the generated or lifted code out of git if it embeds
   game data; hand-written C++ that only encodes the program logic needs a
   decision from the maintainer (it is derived from game microcode).
5. Verify, then measure (below), then do `0x1400`.

## Hard constraints

- **Bit-exact**, including cycle counts, flags, VU1 memory and XGKICK output.
  Apply `scripts/maintainer/dev/vu1-verify.patch` on top of the series and run
  with `PS2X_VU1_VERIFY=1`; expect 0 `[vu1verify]` mismatches over a run into
  gameplay.
- **Host rounding mode:** the interpreter runs under `FE_TOWARDZERO`, and
  queued PATH1 draws carry the mode. Anything lifted must keep this
  (`docs/NOTES.md`, "Trap found on the way").
- VU values are normalized: denormals flush to signed zero, Inf/NaN clamp to
  signed max (`normalizeOperand`); results classify zero/underflow/overflow
  into MAC and status as in `normalizeFmacResult`/`updateFmacFlags`.
- Fixes follow PS2 hardware behaviour, not KFIV hacks. New debug output is off
  by default behind an env var. Never edit `patches/*.patch` by hand.

## Dev loop

```sh
./scripts/maintainer/dev-build.sh      # first build ~5 min, runtime-only change ~5 s
# census (also regenerates the image dump):
mkdir -p runs/census && ./scripts/maintainer/dev-run.sh runs/census 3300 \
  PS2X_VU1_CENSUS=$PWD/runs/census/vu1 \
  PS2X_INPUT="1100:START:10,1500:CROSS:10,1900:CROSS:10"
# benchmark (flips/s over ticks 3000-3599):
./scripts/maintainer/dev-run.sh runs/perf 3700 PS2X_STATS=1 \
  PS2X_INPUT="1100:START:10,1500:CROSS:10,1900:CROSS:10"
grep -E "tick=3[0-5][0-9][0-9] " runs/perf/log.txt | sed -E 's/.*flips\/s=//' \
  | awk '{s+=$1;n++} END {print s/n, n}'
```

Edit in `~/src/PS2Recomp-kfiv` (branch `kfiv`), commit one change per commit,
export with `scripts/maintainer/export-patches.sh ~/src/PS2Recomp-kfiv kfiv`.

Gotchas:
- `ps2xRuntime/include/runtime/ps2_vu1.h` is included by `ps2_runtime.h`, which
  the generated game code includes, so editing it recompiles everything
  (minutes). Keep new state in `.cpp` files or a header the game code does not
  include.
- The census only dumps periodically and at destruction; the runner may exit
  without running static destructors, which is why it dumps every 16k runs.
- Profiling without `perf`: sample the `GameThread` with
  `eu-stack -1 -p <tid>` in a loop (find the tid in `/proc/<pid>/task/*/comm`).
  Sampling the process id alone only shows the idle main thread.
- Keep `runs/` out of git (frame dumps, logs, dumped microcode).

## After this (roadmap, issue #9)

Once the VU1 is fast, the GS (self-feedback strips, #2) becomes the limit.
Then: a GPU `GSRasterBackend` behind a flag with the CPU backend kept as the
reference (Stage B, resolve #3 first), and structured output from the lifted
programs into it (Stage C). Stage D (reconstructing scenes from game data) is
not a goal until C exists.
