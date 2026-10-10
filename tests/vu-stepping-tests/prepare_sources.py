# SPDX-License-Identifier: GPL-3.0-or-later
#!/usr/bin/env python3
"""Derive test-only VU copies from explicit candidate and checked reference trees.

Inputs are read-only. Generated runtime sources belong exclusively in an
external build tree; neither a reference snapshot nor retail data is bundled.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Also support a standalone relocated copy of this package. A zip checkout
# has no .git directory, so recognize this repository's tracked layout too.
REPOSITORY = next((parent for parent in HERE.parents
    if (parent / ".git").exists() or
       ((parent / "AGENTS.md").is_file() and (parent / "tests").is_dir())), HERE)
MANIFEST = HERE / "reference39-hashes.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def inside(path, directory):
    return path == directory or directory in path.parents


def read_runtime(directory, files):
    result = {}
    for relative in files:
        path = directory / relative
        if not path.is_file():
            raise ValueError(f"Missing runtime source: {path}")
        data = path.read_bytes()
        # Git's line-ending conversion is the only accepted reference variation.
        canonical = data.replace(b"\r\n", b"\n")
        result[relative] = (canonical.decode("utf-8"), {
            "sha256": digest(data), "lf_sha256": digest(canonical)
        })
    return result


def replay_access(text, reference=False):
    state = "VU1StateRef" if reference else "VU1State"
    marker = f"    {state} &state() {{ return m_state; }}"
    if text.count(marker) != 1 or "restoreForReplay" in text or "verifyDecodePair" in text:
        raise ValueError("VU header has changed: cannot insert test-only replay access")
    return text.replace(marker,
        f"    void restoreForReplay(const {state} &state) {{ m_state = state; "
        "m_cycle = state.cycles; resetScheduler(); }\n\n"
        "    void verifyDecodePair(uint32_t upper, uint32_t lower, uint32_t *out) const;\n"
        "    static std::size_t verifyDecodedSize();\n\n" + marker)


def scalar_classifier(name):
    # Literal scalar classification used by patch 39's normalizeFmacDouble.
    # It does not change the reference interpreter's production entry points.
    return f"""
void vu_verify_classify_{name}(const double *in, uint8_t *out)
{{
    for (uint32_t lane = 0; lane < 2u; ++lane)
    {{
        const double magnitude = std::fabs(in[lane]);
        uint8_t flags = std::signbit(in[lane]) ? 2u : 0u;
        if (magnitude == 0.0) flags |= 1u;
        else if (magnitude > static_cast<double>(std::numeric_limits<float>::max())) flags |= 8u;
        else if (magnitude < static_cast<double>(std::numeric_limits<float>::min())) flags |= 5u;
        out[lane] = flags;
    }}
}}
"""


def classifier_export(text, name):
    if "vu_verify_classify_" in text:
        raise ValueError("Input contains verifier exports; supply production runtime source")
    fallback = scalar_classifier(name)
    if name == "candidate" and re.search(r"\bclassifyFmacDoubleLanes\s*\(", text):
        return ("\n#if defined(__SSE2__) || defined(_M_X64) || "
            "(defined(_M_IX86_FP) && _M_IX86_FP >= 2)\n"
            "void vu_verify_classify_candidate(const double *in, uint8_t *out)\n"
            "{\n    const auto flags = classifyFmacDoubleLanes(_mm_loadu_pd(in));\n"
            "    out[0] = flags[0]; out[1] = flags[1];\n}\n#else\n" + fallback + "#endif\n")
    return fallback


def decoder_export(text, name):
    """Export actual decoded metadata without replacing either decoder."""
    if "verifyDecodePair" in text or "verifyDecodedSize" in text:
        raise ValueError("Input contains descriptor verifier exports; supply production runtime source")
    required = ("decodeInstructionPair", "decoded.upperVfShadowReg", "decoded.readSlots",
                "decoded.writeSlots", "decoded.writeLatencies", "decoded.firstViWrite")
    if not all(token in text for token in required):
        raise ValueError("VU decoder changed: cannot export its ordered hazard metadata")
    cls = "VU1InterpreterRef" if name == "reference" else "VU1Interpreter"
    compact = "decoded.has(DecodedIBit)" in text
    if compact:
        flags, pipeline = "decoded.flags", "decoded.lowerPipeline"
    else:
        if "decoded.upperUsage" not in text or "decoded.lowerUsage" not in text:
            raise ValueError("Unrecognized VU descriptor layout")
        flags = """uint32_t(decoded.iBit) | (uint32_t(decoded.eBit) << 1) |
        (uint32_t(decoded.mBit) << 2) | (uint32_t(decoded.dBit) << 3) |
        (uint32_t(decoded.tBit) << 4) | (uint32_t(decoded.upperUsage.reserved) << 5) |
        (uint32_t(decoded.lowerUsage.reserved) << 6) | (uint32_t(decoded.lowerUsage.waitQ) << 7) |
        (uint32_t(decoded.lowerUsage.waitP) << 8) | (uint32_t(decoded.lowerUsage.delaysNextBranchRead) << 9)"""
        pipeline = "decoded.lowerUsage.pipeline"
    return f"""
void {cls}::verifyDecodePair(uint32_t upper, uint32_t lower, uint32_t *out) const
{{
    const uint32_t code[2] = {{lower, upper}};
    const auto decoded = decodeInstructionPair(reinterpret_cast<const uint8_t *>(code), 0u);
    if (decoded.readSlotCount > 56u || decoded.writeSlotCount > 28u)
        std::abort(); // Refuse to overrun the fixed test-only metadata output.
    std::fill_n(out, 128u, 0u);
    out[0] = decoded.lower; out[1] = decoded.upper;
    out[2] = {flags}; out[3] = {pipeline};
    out[4] = decoded.upperVfShadowReg; out[5] = decoded.readSlotCount;
    out[6] = decoded.writeSlotCount; out[7] = decoded.firstViWrite;
    out[8] = decoded.maxWriteLatency;
    for (uint32_t index = 0; index < decoded.readSlotCount; ++index)
        out[16u + index] = decoded.readSlots[index];
    for (uint32_t index = 0; index < decoded.writeSlotCount; ++index)
    {{
        out[72u + index] = decoded.writeSlots[index];
        out[100u + index] = decoded.writeLatencies[index];
    }}
}}
std::size_t {cls}::verifyDecodedSize() {{ return sizeof(DecodedInstructionPair); }}
"""


def write(destination, relative, text):
    path = destination / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")


def prepare(candidate, reference, output):
    candidate, reference, output = (path.resolve() for path in (candidate, reference, output))
    if candidate == reference:
        raise ValueError("Candidate and patch-39 reference must be separate runtime directories")
    for directory in (REPOSITORY, candidate, reference):
        if inside(output, directory) or inside(directory, output):
            raise ValueError("Generated output must be an external build directory, separate from source trees")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    files = manifest["files"]
    if len(files) != 5:
        raise ValueError("Reference manifest must contain exactly five frozen VU source files")
    reference_source = read_runtime(reference, files)
    for relative, (_, hashes) in reference_source.items():
        if hashes["lf_sha256"] != files[relative]["lf_sha256"]:
            raise ValueError(f"Patch-39 reference hash mismatch: {relative}; "
                f"expected LF SHA256 {files[relative]['lf_sha256']}, got {hashes['lf_sha256']}")
    candidate_source = read_runtime(candidate, files)
    # Upstream's optional static lift is dormant in this verifier, but link its
    # real support implementation whenever the actual candidate core calls it.
    # Keep the original five-file reference manifest and runtime untouched.
    lift_files = ("include/runtime/ps2_vu1_lift.h", "src/lib/vu/ps2_vu1_lift.cpp")
    if re.search(r"\brunLifted\s*\(", candidate_source["src/lib/vu/ps2_vu1_core.cpp"][0]):
        candidate_source.update(read_runtime(candidate, lift_files))
    if not (candidate / "src/lib/ps2_memory.cpp").is_file():
        raise ValueError("Candidate runtime must include the real src/lib/ps2_memory.cpp")

    # Validate and derive everything before writing any output.
    derived = []
    for name, source in (("candidate", candidate_source), ("reference", reference_source)):
        for relative, (text, _) in source.items():
            if name == "reference":
                text = text.replace("VU1Interpreter", "VU1InterpreterRef").replace("VU1State", "VU1StateRef")
                text = text.replace('"runtime/ps2_vu1.h"', '"runtime/ps2_vu1_ref.h"')
                if relative.endswith("ps2_vu1.h"):
                    text = text.replace("PS2_VU1_H", "PS2_VU1_REF_H")
                    relative = "include/runtime/ps2_vu1_ref.h"
            if relative.endswith(("ps2_vu1.h", "ps2_vu1_ref.h")):
                text = replay_access(text, name == "reference")
            elif relative.endswith("ps2_vu1_upper.cpp"):
                if "vu_verify_normalize_" in text:
                    raise ValueError("Input contains verifier exports; supply production runtime source")
                text += (f"\nvoid vu_verify_normalize_{name}(const float *in, float *out) "
                    "{ normalizeOperands(in, out); }\n")
            elif relative.endswith("ps2_vu1_core.cpp"):
                text += classifier_export(text, name)
                text += decoder_export(text, name)
            derived.append((name + "/" + relative, text))
    for relative, text in derived:
        write(output, relative, text)
    write(output, "candidate-lift.cmake", "set(VU_VERIFY_HAS_STATIC_LIFT " +
        ("ON" if all(path in candidate_source for path in lift_files) else "OFF") + ")\n")
    write(output, "source-manifest.json", json.dumps({
        "reference": manifest["reference"],
        "reference_sources": {path: hashes for path, (_, hashes) in reference_source.items()},
        "candidate_sources": {path: hashes for path, (_, hashes) in candidate_source.items()},
        "candidate_static_lift": "actual support TU linked; PS2X_VU1_LIFT=0 in CTest"
            if all(path in candidate_source for path in lift_files) else "not present",
        "candidate_classifier": "production SSE2 helper with scalar non-SSE2 fallback"
            if "classifyFmacDoubleLanes" in candidate_source["src/lib/vu/ps2_vu1_core.cpp"][0]
            else "scalar patch-39 helper mirror (candidate has no vector classifier)",
        "generated_sources": {path: digest(text.encode("utf-8")) for path, text in derived}
    }, indent=2) + "\n")
    print("Prepared candidate and five hash-checked patch-39 VU sources in external build scratch.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate-runtime", type=Path, required=True)
    parser.add_argument("--reference-runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        prepare(arguments.candidate_runtime, arguments.reference_runtime, arguments.output)
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(1, f"VU source preparation failed: {error}\n")
