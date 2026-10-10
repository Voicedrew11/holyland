# Source-authored cooperative native VU catalog proof

This wrapper derives the neighboring `vu-stepping-tests` package in external
build scratch and uses the actual candidate generic generator to produce an
authored ELF, MPG metadata and native catalog. No ELF or generated C++ is bundled.
Every intended native case asserts an issue-counter increase. Complete state,
cycles, memory and PATH1 remain compared with patch39 through host-job quanta.
Opcode/mask/alias cases, group edges, tracked/unmarked stale-cache directions,
callback mutation, VU0/misalignment/short memory and fault/drain boundaries remain.
Five category controls and parser-input unit cases are retained.

Use the three explicit runtime paths documented by `vu-stepping-tests` and the
actual candidate `tools/generate_vu_native.py`. Per-case test counters are enabled
only in this derived correctness build. This proves authored compiled paths;
privately owned retail replay is separate evidence, not part of this package.
The relocated graph passed all seven CTests on native Windows x64/MSVC 19.44,
RelWithDebInfo, including exact native execution, five category controls and
parser inputs. GNU wide-long-double and native Linux proof remain pending.
GPL-3.0-or-later.
