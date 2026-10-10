# SPDX-License-Identifier: GPL-3.0-or-later
"""Add proof-only scheduler friend access in external build scratch."""
import argparse
import hashlib
from pathlib import Path

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    runtime, output = args.runtime.resolve(), args.output.resolve()
    package = Path(__file__).resolve().parent
    for source in (runtime, package):
        if output == source or source in output.parents:
            raise ValueError('Proof output must be outside the runtime and source package')
    header = runtime / 'include/runtime/ee_scheduler.h'
    text = header.read_text(encoding='utf-8')
    anchor = 'private:\n    struct ScheduledEvent'
    method = '    void setGsVideoMode(uint32_t interlaced, uint32_t videoMode);'
    if text.count(anchor) != 1 or text.count(method) != 1:
        raise ValueError('Canonical scheduler does not match the mode-aware API anchors')
    proof = text.replace(anchor, 'private:\n    friend struct FieldModeProof;\n    struct ScheduledEvent', 1)
    destination = output / 'runtime/ee_scheduler.h'
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.h.tmp')
    with temporary.open('w', encoding='utf-8', newline='\n') as stream:
        stream.write(proof)
    temporary.replace(destination)
    print('scheduler_lf_sha256=' + hashlib.sha256(text.encode('utf-8')).hexdigest())

if __name__ == '__main__':
    main()
