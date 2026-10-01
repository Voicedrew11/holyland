# Recompilation workflow (Linux)

End-to-end pipeline for `SLUS_203.18`. Mirrors the upstream workbench flow,
adapted from Windows batch to Linux shell. Game id = boot ELF name.

## 0. Prerequisites

- CMake 3.20+, C++20 compiler (GCC 13+ / Clang 16+), Ninja or Make,
  Python 3 (stdlib only).
- For the **runner** (step 6): X11 dev headers **or**
  `-DPS2X_ENABLE_DEBUG_UI=OFF` (see step 6).
- For step 4 (recommended): Ghidra + JDK 21+ +
  [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded).
  Installed on this machine 2026-09-30: Ghidra 12.1.3 at
  `~/opt/ghidra_12.1.3_PUBLIC` (sha256-verified), Temurin JDK 21.0.2 via
  sdkman (`JAVA_HOME=~/.sdkman/candidates/java/21.0.2-tem`, default java
  left at 17), extension v2.1.37 (12.1.3 build) dropped into
  `Ghidra/Extensions/`. Launch GUI with `~/opt/ghidra_12.1.3_PUBLIC/ghidraRun`.
- Your own disc image of the US release (`SLUS-20318`).

## 1. Build the tools once

```sh
git clone --recurse-submodules https://github.com/ran-j/PS2Recomp.git "$PS2X_REPO"
cmake -S "$PS2X_REPO" -B "$PS2X_BUILD" \
  -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_RUNTIME=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$PS2X_BUILD" --config Release -j"$(nproc)"
# → ps2_analyzer, ps2_recomp
```

`scripts/0*-.sh` resolve these via `PS2X_REPO` / `PS2X_BUILD` env vars,
defaulting to `~/src/PS2Recomp` and `<repo>-build`.

## 2. Extract the boot ELF

```sh
./scripts/01-extract.sh "/path/to/King's Field The Ancient City.iso" <game-dir>
```

Reads `SYSTEM.CNF`, follows `BOOT2`, writes `<game-dir>/SLUS_203.18`.
Needs no external tools (`extract_elf.py` parses ISO9660 directly).
Writes `<game-dir>/game.properties` (ISO path reference — git-ignored).

## 3. Native analyzer (SDK names, not boundaries)

```sh
./scripts/02-analyze.sh <game-dir>
```

Merges the analyzer's `stubs` list into `kfiv/config.toml` (SDK bindings
via the SCE symbol database; Ghidra finds far fewer). Its function
boundaries are unreliable on stripped ELFs — only the stubs carry over.
Reference result: 264 stub bindings.

## 4. Ghidra import + export (once per game, slow import)

1. Import `SLUS_203.18` in Ghidra with the EmotionEngine language,
   run auto-analysis (minutes; keep the project).
2. Add any `missing_functions.txt` addresses via
   `ps2xRecomp/tools/ghidra/CreateFunctionsAt.java`.
3. Run `ps2xRecomp/tools/ghidra/ExportPS2Functions.java` → fresh TOML + CSV.
4. **Normalize + merge** (never use the raw export — it resets `skip` and
   writes stale paths):
   - point `input`/`output` at your local paths,
   - restore `skip` from your skip list,
   - merge analyzer stubs (strip leading `_` to match runtime registration,
     e.g. `_malloc_r` → `malloc_r`), drop names with no runtime handler,
   - validate every handler against
     `ps2xRuntime/include/ps2_call_list.h` (`PS2_SYSCALL_LIST`/`PS2_STUB_LIST`).

## 5. Recompile

```sh
./scripts/03-recompile.sh <game-dir>
```

Expect `errors: 0`. If errors concentrate in one `function=`, that's a
runaway boundary: add the recompiler's `sub_XXXXXXXX` name (not Ghidra's
`FUN_...`) to the skip list and re-run from step 4-export.
Reference result: 3208 processed / 3019 recompiled / 189 stubs / errors 0.

## 6. Build the runner

```sh
./scripts/04-build-runner.sh <game-dir>
```

Clones PS2Recomp into `<game-dir>/_build`, copies generated sources in,
builds `ps2EntryRunner`. raylib is linked unconditionally by the host
backend, so X11 headers are mandatory on Linux (Fedora:
`sudo dnf install libXinerama-devel libXrandr-devel libXcursor-devel
libXi-devel`). `-DPS2X_ENABLE_DEBUG_UI=OFF` only drops the imgui layer —
it does not remove the X11 requirement.

## 7. Run (needs the disc image)

```sh
./scripts/05-run.sh <game-dir> "/path/to/King's Field The Ancient City.iso"
```

## Iteration loop

- `[guest-branch:missing-target] op=J/JAL, target has no function` →
  add address to `missing_functions.txt`, re-run export → recompile → run.
- `op=JALR target=0x0` → indirect call through null pointer; **not** a
  discovery gap. Look upstream for a failed allocation/uninitialized table.
- Hang with no error → PC histogram in `PS2Runtime::dispatchLoop`
  (recompiled functions loop internally, so the dispatcher iterates rarely).
- Fix order: hard blockers (`function not found`, syscall TODOs, critical
  IO stubs) → temporary `ret0`/`ret1`/`reta0` return-stubs only to classify
  importance → promote to real implementations → move per-game hacks into a
  `PS2_REGISTER_GAME_OVERRIDE` module keyed by ELF metadata.
- Re-test from cold boot after each batch.
