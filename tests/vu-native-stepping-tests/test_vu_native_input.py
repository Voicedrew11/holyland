# SPDX-License-Identifier: GPL-3.0-or-later
"""Asset-free ELF/MPG parser and invalid-output controls (Python 3.8+)."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import vu_native_input
from vu_native_input import CODE_SIZE, read_elf, reconstruct_elf


def elf_with_uploads(uploads):
    data = bytearray(64)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<H', data, 18, 8)
    offsets = []
    for command, payload in uploads:
        offsets.append(len(data))
        data.extend(struct.pack('<I', command))
        data.extend(payload)
    return bytes(data), offsets


def digest(data):
    return hashlib.sha256(data).hexdigest()


def parse(data, offsets):
    return reconstruct_elf(data, digest(data), offsets)[0]


class NativeInputTests(unittest.TestCase):
    def test_ordered_overlap_and_zero_fill(self):
        data, offsets = elf_with_uploads([(0x4a020002, bytes(range(16))), (0x4a010003, b'overlaid')])
        code = parse(data, offsets)
        self.assertEqual(code[16:24], bytes(range(8)))
        self.assertEqual(code[24:32], b'overlaid')
        self.assertEqual(code[:16] + code[32:], bytes(CODE_SIZE - 16))
        self.assertEqual(parse(data, offsets[::-1])[16:32], bytes(range(16)))

    def test_irq_num_zero_imm_mask_wrap_and_exact_end(self):
        payload = bytes(range(256)) * 8
        data, offsets = elf_with_uploads([(0xca00ffff, payload)])
        code = parse(data, offsets)
        self.assertEqual(code[-8:], payload[:8])
        self.assertEqual(code[:2040], payload[8:])
        self.assertEqual(code[2040:-8], bytes(CODE_SIZE - 2048))
        exact, exact_offsets = elf_with_uploads([(0x4a0107ff, b'exactend')])
        self.assertEqual(parse(exact, exact_offsets)[-8:], b'exactend')

    def test_full_image_coverage(self):
        uploads = [(0x4a000000 | block * 256, bytes([block + 1]) * 2048) for block in range(8)]
        data, offsets = elf_with_uploads(uploads)
        self.assertEqual(parse(data, offsets), b''.join(payload for _, payload in uploads))

    def test_hash_format_opcode_bounds_and_offset_types(self):
        data, offsets = elf_with_uploads([(0x4a010000, b'payload8')])
        for pin in [None, '0' * 64, 'g' * 64, digest(data)[:-1]]:
            with self.assertRaises(ValueError):
                reconstruct_elf(data, pin, offsets)
        for invalid in [offsets + offsets, [-4], [1], [True], [2.5], [], [len(data)], [len(data) - 4]]:
            with self.assertRaises(ValueError):
                parse(data, invalid)
        wrong = bytearray(data)
        struct.pack_into('<I', wrong, offsets[0], 0x49010000)
        with self.assertRaisesRegex(ValueError, 'MPG command'):
            parse(wrong, offsets)
        with self.assertRaisesRegex(ValueError, 'payload exceeds'):
            parse(data[:-1], offsets)
        with self.assertRaisesRegex(ValueError, 'command exceeds'):
            parse(data, [len(data)])
        with self.assertRaisesRegex(ValueError, 'MIPS ELF'):
            parse(b'bad ELF' + data[7:], offsets)

    def test_required_typed_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, offsets = elf_with_uploads([(0x4a010000, b'payload8')])
            elf, metadata = root / 'authored.elf', root / 'uploads.json'
            elf.write_bytes(data)
            valid = {'version': 1, 'unit': 'VU1', 'code_size': CODE_SIZE, 'initialization': 'zero',
                'elf_sha256': digest(data), 'uploads': [{'header_offset': hex(offsets[0])}],
                'expected_image_sha256': digest(parse(data, offsets))}
            metadata.write_text(json.dumps(valid), encoding='utf-8')
            self.assertEqual(read_elf(elf, None, [], metadata)[0], parse(data, offsets))
            with self.assertRaisesRegex(ValueError, 'not both'):
                read_elf(elf, digest(data), [], metadata)
            for invalid in [[], None, {**valid, 'version': True}, {**valid, 'code_size': True},
                            {**valid, 'expected_image_sha256': None}, {**valid, 'expected_image_sha256': 'g' * 64},
                            {**valid, 'expected_image_sha256': '0' * 64}, {**valid, 'elf_sha256': '0' * 64},
                            {**valid, 'uploads': [{'header_offset': True}]}, {**valid, 'uploads': None}]:
                metadata.write_text(json.dumps(invalid), encoding='utf-8')
                with self.assertRaises(ValueError):
                    read_elf(elf, None, [], metadata)

    def test_cli_bad_input_does_not_modify_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, offsets = elf_with_uploads([(0x4a010000, b'payload8')])
            elf, metadata = root / 'authored.elf', root / 'uploads.json'
            elf.write_bytes(data)
            metadata.write_text(json.dumps({'version': 1, 'unit': 'VU1', 'code_size': CODE_SIZE,
                'initialization': 'zero', 'elf_sha256': '0' * 64,
                'uploads': [{'header_offset': offsets[0]}], 'expected_image_sha256': digest(parse(data, offsets))}), encoding='utf-8')
            output, manifest = root / 'catalog.inc', root / 'catalog.json'
            command = [sys.executable, str(Path(vu_native_input.__file__).with_name('generate_vu_native.py')),
                '--runtime', str(root / 'unused-runtime'), '--elf', str(elf), '--metadata', str(metadata),
                '--output', str(output), '--manifest', str(manifest)]
            for existing in (False, True):
                if existing:
                    output.write_bytes(b'original catalog')
                    manifest.write_bytes(b'original provenance')
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Owned ELF SHA256 does not match', result.stderr)
                if existing:
                    self.assertEqual(output.read_bytes(), b'original catalog')
                    self.assertEqual(manifest.read_bytes(), b'original provenance')
                else:
                    self.assertFalse(output.exists())
                    self.assertFalse(manifest.exists())


if __name__ == '__main__':
    unittest.main()
