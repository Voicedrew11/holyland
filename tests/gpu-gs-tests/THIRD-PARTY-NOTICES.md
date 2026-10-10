# Source and license notices

The fixture C++, CMake and PowerShell setup sources are GPL-3.0-only.
[LICENSE](LICENSE) contains the upstream runtime's GPL version 3 text.
The fixture input is synthetic. No game assets,
generated retail code, memory cards, command recordings, frame captures,
executables or dependency binaries belong in this package.

The setup script retrieves the following separately licensed source. These
licenses do not change because the source is linked into the test program.

| Component and pinned source | License | Included notice |
| --- | --- | --- |
| paraLLEl-GS `cc6184af7e0c03da603045ca371ffa5dae9b0655`, Copyright 2024 Arntzen Software AS; contributors Hans-Kristian Arntzen and Runar Heyer | LGPL-3.0-or-later (`LGPL-3.0+` in upstream SPDX) | [LGPL text](licenses/parallel-gs-LGPL-3.0.txt), together with the incorporated [GPL text](LICENSE) |
| Granite `16e7395f6a4858c1783dbf6f521f90b9d5f82ac5`, Copyright 2017–2026 Hans-Kristian Arntzen | MIT | [Granite notice](licenses/Granite-MIT.txt) |
| volk `47cddf7ed97b94118a08aacb548a411188e016cc`, Copyright 2018–2026 Arseny Kapoulkine | MIT | [volk notice](licenses/volk-MIT.txt) |
| Vulkan-Headers `11d6898377797e07dbd543aaaa367e4465074597`, Copyright 2015–2026 The Khronos Group Inc. | Per-file Apache-2.0, MIT, or Apache-2.0 OR MIT | [Repository notice](licenses/Vulkan-Headers-NOTICE.md), [Apache text](licenses/Vulkan-Headers-Apache-2.0.txt), [MIT text](licenses/Vulkan-Headers-MIT.txt) |

The included notices were copied verbatim from these pinned checkouts. Keep
their copyright, SPDX and license headers in the corresponding source files.
The local `parallel-gs-small-transfers.patch` modifies LGPL-covered source and
is supplied as source alongside the exact upstream commit and setup steps.

The minimal configuration does not initialize or use optional compiler,
renderer, SDL, audio, compression or capture submodules. Granite's checkout
still contains other third-party source headers and notices; retain those when
redistributing the full checkout. Enabling additional components requires their
own source and license inventory.

For a source bundle, retain the fixture sources, this setup script, the patch,
the exact source pins, and all applicable upstream notices. Retain the prepared
dependency source and local changes when packaging a linked executable so its
corresponding source can be rebuilt; the static library alone is insufficient.
This package currently ships source and notices only. Its build instructions
are not a binary distribution or a claim that unrelated packaging satisfies
all license terms.
