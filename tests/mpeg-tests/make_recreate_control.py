# SPDX-License-Identifier: GPL-3.0-or-later
"""Preserve old Create registrations only in a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
marker = "            g_mpeg_stub_state.callbacksByMpeg.erase(param_1);\n"
if source.count(marker) != 1:
    raise SystemExit("Expected exactly one Create callback clear")
source = source.replace(marker, "", 1)
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
with destination.open("w", encoding="utf-8", newline="\n") as output:
    output.write(source)
