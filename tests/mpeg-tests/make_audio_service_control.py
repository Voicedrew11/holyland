# SPDX-License-Identifier: GPL-3.0-or-later
"""Omit only rejected-audio UPDATE service in a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
marker = "                    if ((type == kMpegStrPCM || type == kMpegStrADPCM) &&"
if source.count(marker) != 1:
    raise SystemExit("Expected exactly one rejected-audio service block")
start = source.index(marker)
cursor = source.index("{", start)
depth = 1
while depth:
    cursor += 1
    depth += (source[cursor] == "{") - (source[cursor] == "}")
source = source[:start] + source[cursor + 2:]
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
with destination.open("w", encoding="utf-8", newline="\n") as output:
    output.write(source)
