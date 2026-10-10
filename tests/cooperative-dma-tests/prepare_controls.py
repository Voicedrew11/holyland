# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive three meaningful negative controls from explicit actual sources."""
import argparse
import hashlib
import json
from pathlib import Path

def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError('Review control anchor: ' + before)
    return text.replace(before, after)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    runtime, output = args.runtime.resolve(), args.output.resolve()
    for protected in (runtime, Path(__file__).resolve().parent):
        if output == protected or output in protected.parents or protected in output.parents:
            raise ValueError('Controls belong in external build scratch')
    memory = (runtime / 'src/lib/ps2_memory.cpp').read_text(encoding='utf-8')
    parser_source = (runtime / 'src/lib/ps2_vif1_interpreter.cpp').read_text(encoding='utf-8')
    chain = replace_once(memory,
        '                const size_t acceptedBytes = p.chainData.size();\n'
        '                m_vif1Incoming.emplace_back(std::move(p.chainData));\n'
        '                m_vif1AcceptedBytes += acceptedBytes;',
        '                processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));')
    parser_copy = replace_once(parser_source,
        '            if (m_vif1Input.empty())\n'
        '            {\n'
        '                // No parser has a live pointer at this safepoint. The first\n'
        '                // owned chunk can become the active allocation directly;\n'
        '                // later chunks still append across command/DMA boundaries.\n'
        '                m_vif1Input = std::move(incoming);\n'
        '            }\n'
        '            else\n'
        '                m_vif1Input.insert(m_vif1Input.end(), incoming.begin(), incoming.end());',
        '            m_vif1Input.insert(m_vif1Input.end(), incoming.begin(), incoming.end());')
    old_offset = replace_once(parser_source,
        'const uint32_t srcStride = (srcIndex - 1u) * bytesPerVector;',
        'const uint32_t srcStride = srcIndex * bytesPerVector;')
    outputs = {'chain_copy.cpp': chain, 'parser_copy.cpp': parser_copy, 'old_offset.cpp': old_offset}
    output.mkdir(parents=True, exist_ok=True)
    for name, text in outputs.items():
        with (output / name).open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(text)
    (output / 'source-manifest.json').write_text(json.dumps({'schema': 1,
        'runtime_inputs_lf_sha256': {'memory': hashlib.sha256(memory.encode()).hexdigest(),
                                   'parser': hashlib.sha256(parser_source.encode()).hexdigest()},
        'control_sha256': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in outputs.items()}}, indent=2) + '\n')

if __name__ == '__main__':
    main()
