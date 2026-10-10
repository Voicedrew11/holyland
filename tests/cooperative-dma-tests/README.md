# Cooperative DMA/VIF input ownership

Set `PS2RECOMP_DIR` to the full patched source checkout and build externally.
The actual allocator, transfer snapshots, VIF parser and GIF arbiter are compiled.
Only GS rasterization and the VU execution callback are authored substitutes.
Independent literal V4 values check the exact input boundary; split commands,
normal/chain/scratchpad/FIFO snapshots, epoch/cancel/reentry and owned allocation
identity assertions remain. The runtime-linked suite separately proves real VU
jobs and scheduler completion tokens.

Three controls are derived from the actual supplied source into build scratch:
chain-copy and parser-copy revert only allocation transfers, while old-offset
reintroduces the exact-boundary UNPACK defect. Each must fail its intended
assertion category. No prior runtime implementation is shipped. The relocated
graph passed all six CTests on native Windows x64/MSVC 19.44, RelWithDebInfo.
GNU strict-FP flags are supplied, not a Linux success claim.
GPL-3.0-or-later.
