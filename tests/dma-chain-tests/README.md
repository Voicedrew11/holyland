# Complete DMA source-chain regression

This source-only package compiles the actual PS2Memory chain walker, VIF0/VIF1
interpreter, GIF arbiter, GS frontend, CPU GS backend, and GS local-memory code.
Synthetic source chains end with VIF DIRECT carrying a PACKED A_D GS FINISH.
The fixture checks the resulting FINISH bit through actual GS CSR MMIO, then
acknowledges it through the actual write-one-to-clear path. No graphics parser
or CSR handling is substituted.

```powershell
cmake -S tests/dma-chain-tests -B C:/KFIV-dev/test-builds/dma-chain-tests -G "Visual Studio 17 2022" -A x64 `
  -DPS2RECOMP_DIR=C:/path/to/patched/PS2Recomp
cmake --build C:/KFIV-dev/test-builds/dma-chain-tests --config RelWithDebInfo --parallel 2
ctest --test-dir C:/KFIV-dev/test-builds/dma-chain-tests -C RelWithDebInfo --output-on-failure -V
```

Only `PS2RECOMP_DIR` is required. There are no workspace defaults; no existing
runtime build, GPU, window, generated guest code, or disc data is needed.
CTest fixes the CPU backend to one worker and disables recording/tracing.

The fixed target must pass. The control restores only the previous 4096-tag
limit in a build-directory copy of `ps2_memory.cpp`. It uses all other sources
unchanged and runs the same assertions. CTest marks that target `WILL_FAIL`;
inspect its output to see the missing FINISH and truncated MSCAL cases.
Configuration fails if the controlled replacement no longer matches the
current walker, requiring a review of the control.

Coverage includes:

- Finite 4095-, 4096-, 4097-, 4487-, and 10000-tag VIF1 chains, using CNT,
  mixed CNT/NEXT, and TTE/NEXT layouts. The NEXT cases jump over synthetic
  early-stop tags, so accidental sequential traversal cannot pass.
- Tail FINISH delivery and acknowledgment, DMA completion, terminal TADR,
  and exact MSCAL callback counts. A 4487-tag case uses a separate zero-QWC
  END following the FINISH-bearing CNT.
- 4097- and 10000-tag GIF chains reaching the actual GS frontend, and a
  10000-tag VIF0 chain whose tail MARK reaches the actual VIF interpreter.
- 5000 calls to the same subroutine, both single-level and nested. Different
  return-stack state must allow each revisit to reach the finite tail.
- Tag IRQ with and without TIE: payload execution precedes the conditional
  stop, and CHCR.TAG records the last executed tag.
- Empty zero-QWC END traversal and self-NEXT, two-tag NEXT, and CALL cycles.
  Cycles must return promptly with bounded payload execution and no FINISH.

The MSCAL callback counts real VIF command delivery; VU microprogram execution
is substituted by this counter. FINISH uses DIRECT, so its GS delivery needs
no VU execution. The synthetic chains do not reproduce a complete game's
rendering, DMA timing, or PS2 hardware equivalence. Entirely empty non-TTE
chains queue no payload; that case tests termination and tag state without
claiming a change to their pre-existing completion semantics.

Native Windows MSVC 19.44 x64 RelWithDebInfo validation passed all 245 fixed
checks and both CTest entries. The prior-limit control produced 59 failed
checks, including lost real GS FINISH at the 4097-tag boundary. Linux and
other platforms have not been run.

This package is GPL-3.0-only, matching the PS2Recomp source it compiles. See
[the tests license](../LICENSE).
