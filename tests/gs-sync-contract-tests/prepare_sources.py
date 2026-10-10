# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive SDK negative controls from the caller's actual canonical runtime."""
from pathlib import Path
import argparse
import hashlib
import json


def exactly_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('Canonical SDK contract anchor missing or ambiguous: ' + before[:90])
    return source.replace(before, after)


def remove_resume_sampling(source):
    begin = source.index('    void WaitVSyncTick(')
    end = source.index('\n    void ', begin + 5)
    function = source[begin:end]
    marker = '        if (fixedResult < 0)\n        {'
    if function.count(marker) != 1:
        raise ValueError('Expected one negative-result continuation sampler in actual WaitVSyncTick')
    start = function.index(marker)
    opening = function.index('{', start)
    depth = 1
    at = opening + 1
    while depth:
        depth += (function[at] == '{') - (function[at] == '}')
        at += 1
    if 'ee.waitVSync(' not in function[start:at] or 'csr.load(' not in function[start:at]:
        raise ValueError('Unexpected continuation sampler body')
    if function[at:at + 1] == '\n':
        at += 1
    stripped = function[:start] + function[at:]
    return source[:begin] + stripped + source[end:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    scheduler_path = args.runtime / 'src/lib/Kernel/EeScheduler.cpp'
    interrupt_path = args.runtime / 'src/lib/Kernel/Syscalls/Interrupt.cpp'
    scheduler = scheduler_path.read_text(encoding='utf-8')
    interrupt = interrupt_path.read_text(encoding='utf-8')
    current = ': static_cast<int>((m_runtime.memory().gs().csr.load(std::memory_order_acquire) >> 13u) & 1u);'
    old = ': static_cast<int>((tick - 1u) & 1u);'
    sampled = ('const uint64_t sampledCsr = m_runtime.memory().gs().csr.load(std::memory_order_acquire);\n'
               '                std::memcpy(m_rdram + physical, &sampledCsr, sizeof(sampledCsr));')
    tick = 'std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));'
    outputs = {
        'csr_control_scheduler.cpp': exactly_once(scheduler, sampled, tick),
        'field_control_scheduler.cpp': exactly_once(scheduler, current, old),
        'wake_control_interrupt.cpp': remove_resume_sampling(interrupt),
    }
    # Validate all anchors before touching any output; authored positive tests
    # compile the unchanged actual canonical sources, not a patched copy.
    args.output.mkdir(parents=True, exist_ok=True)
    for name, source in outputs.items():
        with (args.output / name).open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(source)
    manifest = {'runtime_inputs': {
        'src/lib/Kernel/EeScheduler.cpp': hashlib.sha256(scheduler.encode('utf-8')).hexdigest(),
        'src/lib/Kernel/Syscalls/Interrupt.cpp': hashlib.sha256(interrupt.encode('utf-8')).hexdigest(),
    }, 'generated_controls': {
        name: hashlib.sha256(source.encode('utf-8')).hexdigest() for name, source in outputs.items()
    }}
    (args.output / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
