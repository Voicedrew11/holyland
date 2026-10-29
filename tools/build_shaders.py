#!/usr/bin/env python3
"""Compiles the native GS's shaders (port/gs/native/shaders/) to SPIR-V and writes them into
port/gs/native/gsn_shaders.h, which is committed: building the port needs no shader compiler, only
changing a shader does.

    python tools/build_shaders.py [--glslang PATH]

glslang is Khronos' glslangValidator (or `glslang`): from the Vulkan SDK, a distribution's glslang
package, or built from third_party/parallel-gs/Granite/third_party/glslang. Without --glslang it is
looked for as $GLSLANG, then on the PATH.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = os.path.join(ROOT, "port", "gs", "native", "shaders")
OUT = os.path.join(ROOT, "port", "gs", "native", "gsn_shaders.h")


# Shaders built a second time with a define, under a name of their own: (source, name, defines)
VARIANTS = [
    ("nd_000.vert", "nd_000_oracle.vert", ["-DND_ORACLE"]),  # the vertex oracle (gsn_nd_oracle.inc)
    ("nd_280.vert", "nd_280_oracle.vert", ["-DND_ORACLE"]),  # ... for program 0x280 (gsn_nd_280.inc)
    ("nd_700.vert", "nd_700_oracle.vert", ["-DND_ORACLE"]),  # ... for program 0x700 (gsn_nd_700.inc)
    ("nd_000.vert", "sa_000.vert", ["-DND_STANDALONE"]),  # P8's scene: the same, its own depth (gsn_standalone.inc)
    ("nd_000.vert", "pc_000.vert", ["-DND_STANDALONE", "-DPC_DRAW"]),  # --renderer fast: a batch from its mesh (gsn_pc_draw.inc)
    ("nd_000.vert", "pc_000_jitter.vert", ["-DND_STANDALONE", "-DPC_DRAW", "-DND_JITTER"]),
    ("nd_280.vert", "pc_280.vert", ["-DND_STANDALONE", "-DPC_DRAW"]),
    ("nd_280.vert", "pc_280_jitter.vert", ["-DND_STANDALONE", "-DPC_DRAW", "-DND_JITTER"]),
    ("nd_700.vert", "pc_700.vert", ["-DND_STANDALONE", "-DPC_DRAW"]),
    ("nd_700.vert", "pc_700_jitter.vert", ["-DND_STANDALONE", "-DPC_DRAW", "-DND_JITTER"]),
    ("nd_280.vert", "sa_280.vert", ["-DND_STANDALONE"]),  # ... program 0x280's in the scene
    ("nd_700.vert", "sa_700.vert", ["-DND_STANDALONE"]),  # ... program 0x700's in the scene
    ("nd_water.vert", "sa_water.vert", ["-DND_STANDALONE"]),  # ... the reflection's water in the scene (T3)
    # ... with an upscaler's sub-pixel jitter (sa_jitter_on): their own, so that without one the scene's are as before
    ("nd_000.vert", "sa_000_jitter.vert", ["-DND_STANDALONE", "-DND_JITTER"]),
    ("nd_280.vert", "sa_280_jitter.vert", ["-DND_STANDALONE", "-DND_JITTER"]),
    ("nd_700.vert", "sa_700_jitter.vert", ["-DND_STANDALONE", "-DND_JITTER"]),
    ("nd_water.vert", "sa_water_jitter.vert", ["-DND_STANDALONE", "-DND_JITTER"]),
    ("sa_depth.frag", "sa_depth_ms.frag", ["-DSA_MS"]),  # ... the scene's depth into a multisampled Z buffer
    ("sa_resample.frag", "sa_resample_ms.frag", ["-DSA_MS"]),
    ("sa_copy_run.frag", "sa_copy_run_ms.frag", ["-DSA_MS"]),  # ... a run's copy by its own triangles, multisampled  # ... T2: from a multisampled image at another scale
    ("sa_resample.frag", "sa_resample_depth.frag", ["-DSA_DEPTH"]),  # ... a depth at another scale
    ("sa_resample.frag", "sa_resample_depth_ms.frag", ["-DSA_DEPTH", "-DSA_MS"]),  # ... from a multisampled one
    # --native-sampler hardware (docs/port/PC_RENDERER.md, P1): the GS's fragment shaders reading the RGBA8 texture
    ("gs.frag", "gs_hw.frag", ["-DGSN_HW_SAMPLER"]),
    ("sa_sample.frag", "sa_sample_hw.frag", ["-DGSN_HW_SAMPLER"]),
    ("gs_in.frag", "gs_in_hw.frag", ["-DGSN_HW_SAMPLER"]),
    ("gs_in_ms.frag", "gs_in_ms_hw.frag", ["-DGSN_HW_SAMPLER"]),
]


def find_glslang(given):
    for candidate in (given, os.environ.get("GLSLANG"), "glslangValidator", "glslang"):
        if candidate and (os.path.isfile(candidate) or shutil.which(candidate)):
            return candidate
    sys.exit("no glslang: pass --glslang PATH or set GLSLANG")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslang")
    args = parser.parse_args()
    glslang = find_glslang(args.glslang)

    sources = sorted(f for f in os.listdir(SHADERS) if f.endswith((".vert", ".frag", ".comp")))
    lines = [
        "/* The native GS's shaders as SPIR-V, made by tools/build_shaders.py from port/gs/native/shaders/.",
        "   Generated: edit the shaders and run the script again. */",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
    ]
    builds = [(name, name, []) for name in sources] + [v for v in VARIANTS if v[0] in sources]
    idents = []
    with tempfile.TemporaryDirectory() as tmp:
        for source, name, defines in builds:
            out = os.path.join(tmp, name + ".spv")
            cmd = [glslang, "-V", "--target-env", "vulkan1.1", "-I" + SHADERS] + defines + ["-o", out,
                   os.path.join(SHADERS, source)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                sys.exit("%s failed:\n%s%s" % (name, result.stdout, result.stderr))
            data = open(out, "rb").read()
            words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
            ident = "gsn_spv_" + name.replace(".", "_")
            lines.append("static const uint32_t %s[%d] = {" % (ident, len(words)))
            for i in range(0, len(words), 8):
                lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
            lines.append("};")
            lines.append("")
            idents.append(ident)
    # every shader in one table, for the pipeline list (port/gs/native/gsn_pipeline_warm.inc)
    lines.append("/* every shader above, with its name (the pipeline list finds a shader by its SPIR-V: gsn_pipeline_warm.inc) */")
    lines.append("static const struct { const uint32_t *code; size_t size; const char *name; } gsn_spv_all[] = {")
    for ident in idents:
        lines.append("    {%s, sizeof(%s), \"%s\"}," % (ident, ident, ident[len("gsn_spv_"):]))
    lines.append("};")
    lines.append("")
    with open(OUT, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d shaders)" % (os.path.relpath(OUT, ROOT), len(builds)))


if __name__ == "__main__":
    main()
