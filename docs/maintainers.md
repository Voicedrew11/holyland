# Maintainer guide

How the per-game data in `kfiv/` was produced, and how to regenerate it. End
users never need this: they use the committed function map
(see [`building.md`](building.md)).

`kfiv/` holds:

- `config.toml`: recompiler config with the merged SDK stub list;
- `SLUS_203.18.functions.csv`: function map exported from Ghidra (28,008
  records);
- `SLUS_203.18.sha256`: hash of the exact boot ELF these were made from.

## Prerequisites

- Everything in [`building.md`](building.md), plus Ghidra, JDK 21+ and
  [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded)
  (last used with Ghidra 12.1.3, extension v2.1.37). Ghidra and the extension
  just need to be installed; the scripts don't depend on where.

## 1. Native analyzer (SDK names, not boundaries)

```sh
./scripts/maintainer/analyze.sh [game-dir]
```

Writes `<game-dir>/analyzer.toml`. Only its `stubs` list is useful: SDK
bindings found via the SCE symbol database (264 on this ELF; Ghidra finds far
fewer). Its function boundaries are unreliable on stripped ELFs.

## 2. Ghidra import + export (once per game, slow import)

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

## 3. Recompile and verify

```sh
./scripts/02-recompile.sh <game-dir>
```

Expect `errors: 0`. If errors concentrate in one `function=`, that's a
runaway boundary: add the recompiler's `sub_XXXXXXXX` name (not Ghidra's
`FUN_...`) to the skip list and re-run from the Ghidra export.
Reference result (with the Ghidra map): 28008 processed / 27719 recompiled / 289 stubs / errors 0.

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

## Updating the pinned PS2Recomp commit or patches

`scripts/common.sh` pins `PS2X_REF`. `03-build-runner.sh` checks out that
commit and applies `patches/*.patch` in order. To add or refresh a patch, fix
it in your PS2Recomp checkout, then `git diff > patches/NNNN-name.patch`
(a patch must apply cleanly to the pinned commit). Prefer upstreaming fixes to
PS2Recomp and dropping the patch once merged.
