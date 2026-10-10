# SPDX-License-Identifier: GPL-3.0-or-later
"""Source-authored VU1 catalog and synthetic ELF for native-backend proof.

No retail program, executable, trace or asset is read. Generated files belong
in external test scratch; only this source and the C++ cases are publishable.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

SIZE = 16384
NOP = 0x2ff
END = 0x40000000


def upper(op, mask=15, ft=3, fs=2, fd=4):
    return op | (mask << 21) | (ft << 16) | (fs << 11) | (fd << 6)


def special(selector, mask=0, ft=0, fs=0):
    return 0x3c | (selector & 3) | ((selector & 0x7c) << 4) | (mask << 21) | (ft << 16) | (fs << 11)


def lower_special(selector, source, target=0, mask=0):
    return 0x80000000 | special(selector, mask, target, source)


def fsset(value):
    return (0x15 << 25) | (((value >> 11) & 1) << 21) | (value & 0x7ff)


def original_program():
    pairs = [(0, NOP)] * (SIZE // 8)
    occupied = set()

    def emit(pc, sequence):
        for lo, hi in sequence:
            index = (pc // 8) % len(pairs)
            if index in occupied:
                raise ValueError('Authored program overlap')
            occupied.add(index)
            pairs[index] = (lo, hi)
            pc += 8

    # Two instructions before a compiler group edge; full, scalar, broadcast,
    # ACC, cross-product and integer conversions with dependency/alias reads.
    arithmetic = 0x70
    emit(arithmetic, [
        (0, upper(0x28, 15, 3, 2, 4)),
        (0, upper(0x29, 15, 3, 4, 5)),
        (0, upper(0x2d, 7, 5, 2, 5)),
        (0, special(0x29, 15, 3, 5)),
        (0, upper(0x20, 15, 3, 4, 6)),
        (0, upper(0x21, 8, 3, 6, 6)),
        (0, upper(0x22, 15, 3, 6, 7)),
        (0, upper(0x27, 3, 3, 7, 7)),
        (0, upper(0x00, 15, 3, 7, 8)),
        (0, upper(0x1b, 15, 3, 8, 9)),
        (0, special(0x2e, 14, 3, 2)),
        (0, upper(0x2e, 14, 3, 2, 10)),
        (0, special(0x10, 15, 10, 2)),
        (0, special(0x14, 15, 11, 10)),
        (0x3f7ffffe, upper(0x23, 15, 3, 2, 12) | 0x80000000),
        (fsset(0x980), upper(0x28, 15, 3, 12, 12)),
        (0, NOP | END), (0, NOP),
    ])
    pipelines = 0x270
    emit(pipelines, [
        (lower_special(0x38, 2, 3), NOP),
        (0, upper(0x28)), (0, upper(0x2a)),
        (fsset(0xc80), upper(0x28)),
        (lower_special(0x3b, 0), NOP),
        (lower_special(0x70, 2), special(0x1f, 0, 3, 2)),
        (lower_special(0x72, 2), upper(0x28)),
        (lower_special(0x7b, 0), NOP),
        ((0x11 << 25) | 0xabcdef, special(0x1f, 0, 3, 2)),
        (0, NOP | END), (0, NOP),
    ])
    branches = 0x480
    emit(branches, [
        ((0x08 << 25) | (2 << 16) | (1 << 11) | 1, NOP),
        ((0x28 << 25) | (2 << 16) | (1 << 11) | 2, NOP),
        (0, upper(0x28)), (0, upper(0x2a)),
        ((0x21 << 25) | (3 << 16) | 2, NOP),
        (0, upper(0x29)), (0, upper(0x2d)),
        ((0x20 << 25) | 1, NOP | 0x10000000),
        (0, upper(0x28)), (0, NOP | END), (0, NOP),
    ])
    stores = 0x570
    emit(stores, [
        ((1 << 25) | (15 << 21) | (1 << 16) | (2 << 11), upper(0x28)),
        ((0 << 25) | (7 << 21) | (3 << 16) | (1 << 11), upper(0x29)),
        ((5 << 25) | (10 << 21) | (2 << 16) | (1 << 11), NOP),
        ((4 << 25) | (8 << 21) | (4 << 16) | (1 << 11), NOP),
        (lower_special(0x36, 1, 2, 15), NOP),
        (lower_special(0x34, 1, 3, 15), NOP),
        (0, NOP | END), (0, NOP),
    ])
    packets = 0x670
    emit(packets, [
        (lower_special(0x6c, 1), NOP),
        (lower_special(0x3c, 2, 1), NOP),
        (lower_special(0x6c, 1), NOP),
        (lower_special(0x3b, 0), NOP),
        (0, NOP | END), (0, NOP),
    ])
    # The final pair wraps into an authored terminator at address zero.
    wrap = 0x3fe8
    emit(wrap, [(0, upper(0x28)), (0, upper(0x2a)), (0, NOP)])
    emit(0, [(0, NOP | END), (0, NOP)])
    return pairs, {'arithmetic': arithmetic, 'pipelines': pipelines,
                   'branches': branches, 'stores': stores, 'packets': packets,
                   'wrap': wrap, 'unused': 0x3500}


def program():
    pairs, entries = original_program()
    words = []
    selectors = [code for code in range(0x31) if code != 0x2b]
    # Every valid normal and special upper opcode, every destination mask.
    # Registers remain independent here, so a wrong source choice is visible.
    for code in range(0x30):
        for mask in range(16):
            words.append(upper(code, mask, 3, 2, 4))
    for code in selectors:
        for mask in range(16):
            words.append(special(code, mask, 3, 2))
    # Additional destination/source aliases and VF0 cover each valid opcode.
    for code in range(0x30):
        for destination in (2, 3, 0):
            words.append(upper(code, 15, 3, 2, destination))
    for code in selectors:
        words.append(special(code, 15, 2, 2))
        words.append(special(code, 15, 3, 0))
    assert len(words) == 1776
    first = 0x700 // 8
    assert first + len(words) <= 2036
    for index, word in enumerate(words, first):
        assert pairs[index] == (0, NOP)
        pairs[index] = (0, word)
    drain = 0x3fa0
    pairs[drain // 8] = (0, NOP | END)
    pairs[drain // 8 + 1] = (0, NOP)
    entries.update(allOpcodeBegin=first * 8, allOpcodeCount=len(words), allOpcodeDrain=drain)
    return pairs, entries


def generate(directory):
    pairs, entries = program()
    image = b''.join(struct.pack('<II', *pair) for pair in pairs)
    assert len(image) == SIZE
    # Minimal source-authored ELF32/MIPS header. MPG payloads are deliberately
    # independent of loadable EE sections; this fixture exercises extraction.
    elf = bytearray(64)
    elf[:16] = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<HHIIIIIHHHHHH', elf, 16, 2, 8, 1, 0, 0, 0, 0, 52, 0, 0, 0, 0, 0)
    offsets = []
    for first in range(0, len(pairs), 256):
        offsets.append(len(elf))
        command = (0x4a << 24) | first  # NUM zero encodes 256 instructions.
        elf.extend(struct.pack('<I', command))
        elf.extend(image[first * 8:(first + 256) * 8])
    metadata = {'version': 1, 'unit': 'VU1', 'initialization': 'zero',
                'elf_sha256': hashlib.sha256(elf).hexdigest(), 'code_size': SIZE,
                'expected_image_sha256': hashlib.sha256(image).hexdigest(),
                'uploads': [{'header_offset': value} for value in offsets]}
    header = ['// Generated from source-authored fixture; keep in external build scratch.\n',
              'namespace authoredVU {\ninline constexpr uint32_t pairs[][2] = {\n']
    header.extend(f'{{0x{lo:08x}u,0x{hi:08x}u}},\n' for lo, hi in pairs)
    header.append('};\n')
    header.extend(f'inline constexpr uint32_t {name} = {pc}u;\n' for name, pc in entries.items())
    header.append('}\n')
    directory.mkdir(parents=True, exist_ok=True)
    (directory / 'authored.elf').write_bytes(elf)
    (directory / 'authored.mpg.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    (directory / 'authored.image').write_bytes(image)
    (directory / 'authored_catalog.inc').write_text(''.join(header), encoding='utf-8')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scratch', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(generate(args.scratch), sort_keys=True))
