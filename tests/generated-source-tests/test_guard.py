#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Authored snippets only: no retail instructions, functions or assets."""
import runpy
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VALIDATE = runpy.run_path(str(ROOT / 'scripts/check-generated-sources.py'))['validate']
STAGE = runpy.run_path(str(ROOT / 'scripts/windows/Prepare-KFIVSources.py'))['stage']


class GeneratedSourceGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Path(self.temp.name) / 'game'
        self.output = self.game / 'output'
        self.output.mkdir(parents=True)
        for name in ('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h',
                     'register_functions.cpp'):
            (self.output / name).write_text('// authored placeholder\n')
        (self.output / 'synthetic.cpp').write_text('float x = FPU_SQRT_S(input);\n')

    def test_accept_patched_sqrt_and_rsqrt(self):
        (self.output / 'rsqrt.cpp').write_text('float x = numerator / FPU_SQRT_S(denominator);\n')
        self.assertEqual(VALIDATE(self.output), (3, 2))

    def test_reject_mixed_old_and_new_output(self):
        for legacy in ('ctx->f[3] = FPU_SQRT_S(ctx->f[8]);',
                       'ctx->f[3] = 1.0f / sqrtf(ctx->f[8]);'):
            with self.subTest(legacy=legacy):
                (self.output / 'stale.cpp').write_text(legacy)
                with self.assertRaisesRegex(ValueError, 'Stale EE SQRT/RSQRT'):
                    VALIDATE(self.output)

    def test_reject_missing_table_or_header(self):
        for name in ('register_functions.cpp', 'ps2_recompiled_stubs.h'):
            with self.subTest(name=name):
                path = self.output / name
                path.unlink()
                with self.assertRaisesRegex(ValueError, 'Missing generated'):
                    VALIDATE(self.output)
                path.write_text('// authored placeholder\n')

    def test_reject_output_without_patched_sites(self):
        (self.output / 'synthetic.cpp').write_text('// empty\n')
        with self.assertRaisesRegex(ValueError, 'No patched EE'):
            VALIDATE(self.output)

    def test_stage_fails_before_touching_checkout(self):
        checkout = Path(self.temp.name) / 'checkout'
        include = checkout / 'ps2xRuntime/include'
        runner = checkout / 'ps2xRuntime/src/runner'
        include.mkdir(parents=True)
        runner.mkdir(parents=True)
        sentinel = include / 'ps2_recompiled_functions.h'
        sentinel.write_text('existing header')
        (self.output / 'stale.cpp').write_text('ctx->f[3] = FPU_SQRT_S(ctx->f[8]);')
        with self.assertRaisesRegex(ValueError, 'Stale EE'):
            STAGE(self.game, checkout)
        self.assertEqual(sentinel.read_text(), 'existing header')
        self.assertEqual(list(runner.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
