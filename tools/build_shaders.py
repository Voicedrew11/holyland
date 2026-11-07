#!/usr/bin/env python3
"""Compiles the PC renderer's shaders (port/gs/pc/shaders/) to SPIR-V and writes them into port/gs/pc/pcr_shaders.h.
The file is committed: building the port needs no shader compiler, only changing a shader does.

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
PC_SHADERS = os.path.join(ROOT, "port", "gs", "pc", "shaders")
PC_OUT = os.path.join(ROOT, "port", "gs", "pc", "pcr_shaders.h")
# the native draws' VU1 maths, shared with the PC renderer's shaders
SHARED = os.path.join(ROOT, "port", "render", "shaders")


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
    build(glslang, PC_SHADERS, PC_OUT, "pcr_spv_", "The PC renderer's shaders", "port/gs/pc/shaders/")


def build(glslang, folder, target, prefix, what, where):
    sources = sorted(f for f in os.listdir(folder) if f.endswith((".vert", ".frag", ".comp")))
    lines = [
        "/* %s as SPIR-V, made by tools/build_shaders.py from %s." % (what, where),
        "   Generated: edit the shaders and run the script again. */",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for name in sources:
            out = os.path.join(tmp, name + ".spv")
            cmd = [glslang, "-V", "--target-env", "vulkan1.1", "-I" + folder, "-I" + SHARED, "-o", out,
                   os.path.join(folder, name)]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode != 0:
                sys.exit("%s failed:\n%s%s" % (name, result.stdout, result.stderr))
            data = open(out, "rb").read()
            words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
            lines.append("static const uint32_t %s%s[%d] = {" % (prefix, name.replace(".", "_"), len(words)))
            for i in range(0, len(words), 8):
                lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
            lines.append("};")
            lines.append("")
    with open(target, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print("wrote %s (%d shaders)" % (os.path.relpath(target, ROOT), len(sources)))


if __name__ == "__main__":
    main()
