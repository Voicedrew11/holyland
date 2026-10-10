# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare kernel-only differential sources in external build scratch."""
import argparse
import hashlib
import json
from pathlib import Path

REFERENCE_PINS = {
    "iop_kernel.h": "260863c946a687faf5e88c309615de4da319a3028124a97b714468dbf6df0ab5",
    "iop_kernel.cpp": "203c9949793be35f30583612db855544356d1ec1124b96fdf9377e4d321c1002",
}

def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("Unexpected source anchor: " + old)
    return text.replace(old, new, 1)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = args.output.resolve()
    if output == root or root in output.parents:
        raise ValueError("Scratch output must be outside the source package")
    relative = Path("src/emulator/core")
    reference = {name: (args.reference / relative / name).read_text(encoding="utf-8")
                 for name in REFERENCE_PINS}
    for name, expected in REFERENCE_PINS.items():
        observed = hashlib.sha256(reference[name].encode("utf-8")).hexdigest()
        if observed != expected:
            raise ValueError("Pinned reference differs: " + name)
    candidate = {name: (args.candidate / relative / name).read_text(encoding="utf-8")
                 for name in REFERENCE_PINS}
    if "m_idleSelectionValid" not in candidate["iop_kernel.h"]:
        raise ValueError("Candidate does not contain the idle-selection cache")
    friend = "        friend struct KernelStateProof;\n"
    candidate_header = replace_once(candidate["iop_kernel.h"], "    private:\n", "    private:\n" + friend)
    files = {"emulator/core/iop_kernel.h": candidate_header,
             "emulator/core/iop_kernel.cpp": candidate["iop_kernel.cpp"]}
    class_text = reference["iop_kernel.h"][reference["iop_kernel.h"].index("    class IopKernel\n"):]
    class_text = replace_once(class_text, "    private:\n", "    private:\n" + friend)
    class_text = class_text.replace("IopKernel", "IopKernelReference")
    files["iop_kernel_reference.h"] = '#pragma once\n#include "emulator/core/iop_kernel.h"\nnamespace ps2x::iop::detail\n{\n' + class_text
    reference_core = reference["iop_kernel.cpp"].replace('#include "iop_kernel.h"', '#include "iop_kernel_reference.h"')
    reference_core = reference_core.replace('"iop_memory.h"', '"emulator/core/iop_memory.h"').replace('"../iop_emulator_const.h"', '"emulator/iop_emulator_const.h"')
    reference_core = reference_core.replace("IopKernel::", "IopKernelReference::").replace("IopKernelReference::IopKernel(", "IopKernelReference::IopKernelReference(")
    files["iop_kernel_reference.cpp"] = reference_core
    for name, old, new in [
        ("IopKernelNoInvalidate", """    bool IopKernel::dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        m_idleSelectionValid = false;""", """    bool IopKernel::dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {"""),
        ("IopKernelWrongDeadline", "currentCycle < m_idleNextWakeCycle", "currentCycle <= m_idleNextWakeCycle"),
    ]:
        control_core = replace_once(candidate["iop_kernel.cpp"], old, new)
        control_class = candidate_header[candidate_header.index("    class IopKernel\n"):].replace("IopKernel", name)
        files[name + ".h"] = '#pragma once\n#include "emulator/core/iop_kernel.h"\nnamespace ps2x::iop::detail\n{\n' + control_class
        control_core = control_core.replace('#include "iop_kernel.h"', '#include "' + name + '.h"')
        control_core = control_core.replace('"iop_memory.h"', '"emulator/core/iop_memory.h"').replace('"../iop_emulator_const.h"', '"emulator/iop_emulator_const.h"')
        control_core = control_core.replace("IopKernel::", name + "::").replace(name + "::IopKernel(", name + "::" + name + "(")
        files[name + ".cpp"] = control_core
    # All inputs and anchors are validated before publishing any output.
    for name, text in files.items():
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix(destination.suffix + ".tmp")
        with temporary.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        temporary.replace(destination)
    provenance = {"reference_lf_sha256": REFERENCE_PINS,
                  "candidate_lf_sha256": {name: hashlib.sha256(text.encode("utf-8")).hexdigest()
                                          for name, text in candidate.items()}}
    (output / "input-hashes.json").write_text(json.dumps(provenance, indent=2) + "\n")

if __name__ == "__main__":
    main()
