# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise optional native VU catalog generation with an authored ELF.

No game image, generated catalog or compiler output belongs in source control.
The supplied work directory must be new; results and build logs stay there.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path


FILES = (
    'include/runtime/ps2_vu1.h', 'src/lib/vu/ps2_vu1_core.cpp',
    'src/lib/vu/ps2_vu1_upper.cpp', 'src/lib/vu/ps2_vu1_lower.cpp',
    'src/lib/vu/ps2_vu1_detail.h',
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--cmake', type=Path, required=True)
    parser.add_argument('--work', type=Path, required=True)
    args = parser.parse_args()
    root = args.work.resolve()
    root.mkdir()
    source = root / 'runtime'
    source.mkdir()
    for name in FILES:
        target = source / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.runtime / name, target)
    tools = source / 'tools'
    tools.mkdir()
    for name in ('generate_vu_native.py', 'vu_native_input.py'):
        shutil.copyfile(args.tools / name, tools / name)
    (source / 'cmake').mkdir()
    shutil.copyfile(args.module, source / 'cmake/NativeVu.cmake')
    (source / 'dummy.cpp').write_text('int authored_build_graph_dummy() { return 0; }\n')
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.20)\n'
        'project(NativeVuBuildGraph LANGUAGES CXX)\n'
        'add_library(ps2_runtime STATIC dummy.cpp)\n'
        'include(cmake/NativeVu.cmake)\n')

    image = struct.pack('<II', 0x8000033c, 0x000002ff) * 2048
    data = bytearray(64)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<H', data, 18, 8)
    offsets = []
    for index in range(8):
        offsets.append(len(data))
        data.extend(struct.pack('<I', 0x4a000000 | (index * 256)))
        data.extend(image[index * 2048:(index + 1) * 2048])
    elf = root / 'authored.elf'
    elf.write_bytes(data)
    metadata = root / 'authored.json'
    pins = dict(version=1, unit='VU1', initialization='zero', code_size=16384,
                elf_sha256=digest(data), expected_image_sha256=digest(image),
                uploads=[{'header_offset': offset} for offset in offsets])
    metadata.write_text(json.dumps(pins) + '\n')

    checks = []
    def run(label, arguments, success=True):
        completed = subprocess.run([str(args.cmake), *arguments], capture_output=True,
                                   text=True, timeout=120)
        (root / (label + '.log')).write_text(completed.stdout + completed.stderr)
        if (completed.returncode == 0) != success:
            raise RuntimeError(f'{label} exited {completed.returncode}; inspect its log')
        checks.append({'case': label, 'exit_code': completed.returncode})
        return completed.stdout + completed.stderr

    disabled = root / 'disabled'
    run('disabled-configure', ['-S', str(source), '-B', str(disabled),
                               '-DCMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE'])
    run('disabled-build', ['--build', str(disabled), '--config', 'Release'])
    assert not (disabled / 'vu-native').exists()
    run('partial-configure', ['-S', str(source), '-B', str(root / 'partial'),
                             '-DPS2X_VU1_NATIVE_ELF=' + str(elf)], False)
    build = root / 'enabled'
    run('enabled-configure', ['-S', str(source), '-B', str(build),
                             '-DPS2X_VU1_NATIVE_ELF=' + str(elf),
                             '-DPS2X_VU1_NATIVE_METADATA=' + str(metadata)])
    build_args = ['--build', str(build), '--config', 'Release',
                  '--target', 'ps2_vu1_native_catalog']
    run('enabled-generate', build_args)
    catalog = build / 'vu-native/vu1_catalog.inc'
    manifest = build / 'vu-native/vu1_catalog.json'
    original = catalog.read_bytes(), manifest.read_bytes()
    assert json.loads(original[1])['input_code_sha256'] == digest(image)
    before = catalog.stat().st_mtime_ns
    run('unchanged-build', build_args)
    assert catalog.stat().st_mtime_ns == before
    assert (catalog.read_bytes(), manifest.read_bytes()) == original

    # A content mutation in every canonical source/tool must regenerate. The
    # comments preserve semantics and let unchanged catalog bytes be checked.
    for number, name in enumerate((*FILES, 'tools/generate_vu_native.py',
                                  'tools/vu_native_input.py')):
        path = source / name
        time.sleep(1.05)  # Some native build tools compare whole-second mtimes.
        with path.open('a', encoding='utf-8') as output:
            output.write('\n' + ('#' if path.suffix == '.py' else '//') + ' build graph probe\n')
        previous = catalog.stat().st_mtime_ns
        run('source-dependency-' + str(number), build_args)
        assert catalog.stat().st_mtime_ns > previous
    # Ordered metadata and owned ELF are explicit dependencies too.
    for number, path in enumerate((elf, metadata)):
        time.sleep(1.05)
        path.touch()
        previous = catalog.stat().st_mtime_ns
        run('input-dependency-' + str(number), build_args)
        assert catalog.stat().st_mtime_ns > previous
    valid = catalog.read_bytes(), manifest.read_bytes()
    invalid = dict(pins, elf_sha256='0' * 64)
    time.sleep(1.05)
    metadata.write_text(json.dumps(invalid) + '\n')
    log = run('invalid-input-blocks-stale-output', build_args, False)
    assert 'Owned ELF SHA256 does not match' in log
    assert (catalog.read_bytes(), manifest.read_bytes()) == valid
    metadata.write_text(json.dumps(pins) + '\n')
    run('restore-valid-input', build_args)
    # Regenerating in another directory must not add paths/time to output.
    second = root / 'second'
    second.mkdir()
    subprocess.run([sys.executable, str(tools / 'generate_vu_native.py'),
        '--runtime', str(source), '--elf', str(elf), '--metadata', str(metadata),
        '--output', str(second / 'catalog.inc'), '--manifest', str(second / 'manifest.json')],
        check=True, capture_output=True, text=True, timeout=120)
    assert (catalog.read_bytes(), manifest.read_bytes()) == (
        (second / 'catalog.inc').read_bytes(), (second / 'manifest.json').read_bytes())
    result = {'checks': checks, 'deterministic': True, 'authored_inputs_only': True,
              'catalog_sha256': digest(catalog.read_bytes()), 'platform': sys.platform}
    (root / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
