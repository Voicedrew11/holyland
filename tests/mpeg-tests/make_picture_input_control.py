# SPDX-License-Identifier: GPL-3.0-or-later
"""Remove only picture handoff NODATA service from a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
block = """                // The SDK services NODATA after the picture's output DMA.
                // Let the registered input consumer reconcile real DMA
                // progress and prime the next batch after UPDATE completes.
                MpegStreamCallbackEvent input{};
                input.mpegAddr = mpegAddr;
                input.callbackGeneration = playback.callbackGeneration;
                input.streamType = 1u;
                input.callbacks = matchingOrdinaryCallbacks(mpegAddr, 1u);
                if (!input.callbacks.empty())
                    pictureCallbacks.push_back(std::move(input));
"""
if source.count(block) != 1:
    raise SystemExit("Expected exactly one picture input service control block")
source = source.replace(block, "", 1)
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
with destination.open("w", encoding="utf-8", newline="\n") as output:
    output.write(source)
