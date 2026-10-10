#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Rename the supplied actual SPU source into external build scratch for a compiler control."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    runtime = args.runtime_dir.resolve(strict=True)
    output = args.output_dir.resolve()
    package = Path(__file__).resolve().parent
    if output.is_relative_to(runtime) or output.is_relative_to(package):
        raise ValueError("Scratch reference directory must be external to runtime and fixture sources")
    core = runtime / "ps2xIOP/src/emulator/core"
    cpp_path, header_path = core / "iop_spu2.cpp", core / "iop_spu2.h"
    cpp_bytes, header_bytes = cpp_path.read_bytes(), header_path.read_bytes()
    cpp, header = cpp_bytes.decode("utf-8-sig"), header_bytes.decode("utf-8-sig")
    if cpp.count('#include "iop_spu2.h"') != 1 or len(re.findall(r"\bclass\s+IopSpu2\b", header)) != 1:
        raise ValueError("Expected one actual SPU header include and one IopSpu2 declaration")
    if "IopSpu2::advance" not in cpp or "IopSpu2::dmaTransfer" not in cpp:
        raise ValueError("Actual SPU clock and DMA implementation anchors are missing")
    cpp = cpp.replace('#include "iop_spu2.h"', '#include "iop_spu2_ref.h"', 1)
    cpp = re.sub(r"\bIopSpu2\b", "IopSpu2Ref", cpp)
    header = re.sub(r"\bIopSpu2\b", "IopSpu2Ref", header)
    # Only symbol/header names change; comments and third-party notices remain.
    output.mkdir(parents=True, exist_ok=True)
    for name, source in (("iop_spu2_ref.cpp", cpp), ("iop_spu2_ref.h", header)):
        with (output / name).open("w", encoding="utf-8", newline="\n") as target:
            target.write(source)
    manifest = {"transformation": "IopSpu2 -> IopSpu2Ref, header include rename only",
                "source_sha256": hashlib.sha256(cpp_bytes).hexdigest(),
                "header_sha256": hashlib.sha256(header_bytes).hexdigest()}
    (output / "reference-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Generated external SPU compiler reference; production sources unchanged")


if __name__ == "__main__":
    main()
