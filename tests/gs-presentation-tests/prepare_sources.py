# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive the checked prior frontend and exact candidate UploadFrame in scratch."""
import argparse
import hashlib
import json
from pathlib import Path
import re

def read_checked(root, relative, expected):
    data = (root / relative).read_bytes().replace(b'\r\n', b'\n')
    if hashlib.sha256(data).hexdigest() != expected:
        raise ValueError('Prior frontend source changed: ' + relative)
    return data.decode('utf-8')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('candidate-runtime', 'prior-runtime', 'output'):
        parser.add_argument('--' + option, required=True, type=Path)
    args = parser.parse_args()
    candidate, prior, output = (p.resolve() for p in (args.candidate_runtime, args.prior_runtime, args.output))
    for protected in (candidate, prior, Path(__file__).resolve().parent):
        if output == protected or output in protected.parents or protected in output.parents:
            raise ValueError('Derived production source belongs only in external build scratch')
    pins = json.loads((Path(__file__).parent / 'prior-frontend-hashes.json').read_text())['files']
    header = read_checked(prior, 'include/runtime/gs/gs_frontend.h', pins['include/runtime/gs/gs_frontend.h'])
    cpp = read_checked(prior, 'src/lib/gs/gs_frontend.cpp', pins['src/lib/gs/gs_frontend.cpp'])
    if header.count('class GS\n') != 1 or cpp.count('GS::GS()') != 1:
        raise ValueError('Review the actual prior frontend class boundaries')
    header = re.sub(r'\bGS\b', 'GSReference', header[header.index('class GS\n'):header.rindex('#endif')])
    header = '#pragma once\n#include "runtime/gs/gs_frontend.h"\n' + header
    cpp = cpp.replace('#include "runtime/gs/gs_frontend.h"', '#include "gs_frontend_reference.h"')
    cpp = cpp.replace('GS::GS()', 'GSReference::GSReference()').replace('GS::', 'GSReference::')
    runtime_source = (candidate / 'src/lib/ps2_runtime.cpp').read_text(encoding='utf-8')
    if runtime_source.count('static void UploadFrame(') != 1:
        raise ValueError('Review candidate UploadFrame boundary')
    start = runtime_source.index('static void UploadFrame(')
    end = runtime_source.index('PS2Runtime::PS2Runtime()', start)
    outputs = {'gs_frontend_reference.h': header, 'gs_frontend_reference.cpp': cpp,
               'upload_frame.inc': runtime_source[start:end]}
    output.mkdir(parents=True, exist_ok=True)
    for name, text in outputs.items():
        with (output / name).open('w', encoding='utf-8', newline='\n') as stream:
            stream.write(text)
    (output / 'source-manifest.json').write_text(json.dumps({'schema': 1,
        'prior_source_lf_sha256': pins,
        'candidate_upload_source_lf_sha256': hashlib.sha256(runtime_source.encode()).hexdigest(),
        'derived_sha256': {name: hashlib.sha256(text.encode()).hexdigest() for name, text in outputs.items()}}, indent=2) + '\n')

if __name__ == '__main__':
    main()
