#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build a scratch prior-flush Vulkan control; never edit production sources."""
import argparse
from pathlib import Path


def replace_once(source: str, before: str, after: str, label: str) -> str:
    if source.count(before) != 1:
        raise ValueError(f"Expected exactly one {label} anchor")
    return source.replace(before, after, 1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--prior-dir", type=Path,
                        help="Optional frozen pre-optimization source directory")
    args = parser.parse_args()
    gs = args.runtime_dir / "ps2xRuntime/src/lib/gs"
    header = args.runtime_dir / "ps2xRuntime/include/runtime/gs/gs_vulkan_backend.h"
    if args.prior_dir:
        backend = (args.prior_dir / "gs_vulkan_backend.cpp").read_text(encoding="utf-8")
        scanout = (args.prior_dir / "gs_vulkan_scanout.cpp").read_text(encoding="utf-8")
        scanout_header = (args.prior_dir / "gs_vulkan_scanout.h").read_text(encoding="utf-8")
        backend_header = (args.prior_dir / "gs_vulkan_backend.h").read_text(encoding="utf-8")
    else:
        backend = (gs / "gs_vulkan_backend.cpp").read_text(encoding="utf-8")
        scanout = (gs / "gs_vulkan_scanout.cpp").read_text(encoding="utf-8")
        scanout_header = (gs / "gs_vulkan_scanout.h").read_text(encoding="utf-8")
        backend_header = header.read_text(encoding="utf-8")
        backend = replace_once(backend, """        if (submissionFlushed)
        {
            ++reusedSubmissionFlushes;
            return;
        }
""", "", "submission reuse guard")
        backend = replace_once(backend, "    p.flushSubmission();\n", "",
                               "adapter presentation flush")
        backend = replace_once(backend, "presentVulkanGs(p.gs, p.device, request, true)",
                               "presentVulkanGs(p.gs, p.device, request, false)",
                               "adapter presentation reuse")
        scanout = replace_once(scanout, "    if (!alreadyFlushed) gs.flush();",
                               "    (void)alreadyFlushed; gs.flush();",
                               "scanout submission reuse")

    # Both implementations retain the actual library/backend and differ only
    # in explicit submission-flush reuse. Generated files stay in the build.
    sources = {
        "gs_vulkan_backend_ref.cpp": backend,
        "gs_vulkan_backend_ref.h": backend_header,
        "gs_vulkan_scanout_ref.cpp": scanout,
        "gs_vulkan_scanout_ref.h": scanout_header,
    }
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, source in sources.items():
        source = source.replace('"runtime/gs/gs_vulkan_backend.h"', '"gs_vulkan_backend_ref.h"')
        source = source.replace('"gs_vulkan_scanout.h"', '"gs_vulkan_scanout_ref.h"')
        source = source.replace("GSVulkanBackend", "GSVulkanBackendRef")
        source = source.replace("presentVulkanGs", "presentVulkanGsRef")
        source = source.replace("VulkanGsScanoutUnsupported", "VulkanGsScanoutUnsupportedRef")
        with (args.output_dir / name).open("w", encoding="utf-8", newline="\n") as target:
            target.write(source)
    print("Generated scratch prior-flush control; production sources unchanged")


if __name__ == "__main__":
    main()
