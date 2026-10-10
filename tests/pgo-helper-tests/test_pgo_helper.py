# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise profile-merge failure/retry guards without LINK or game inputs."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


HELPER = Path(__file__).resolve().parents[2] / 'scripts/windows/Prepare-KFIVPgo.py'
spec = importlib.util.spec_from_file_location('kfiv_pgo_helper', HELPER)
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


class MergeRetryTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix='kfiv-pgo-helper-')
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name).resolve()
        (self.root / 'train').mkdir()
        (self.root / 'optimized').mkdir()
        self.training = self.root / 'training.pgd'
        self.training.write_bytes(b'authored-unmerged-pgd')
        self.training_bytes = self.training.read_bytes()
        self.pgc = self.root / 'train/authored!1.pgc'
        self.pgc.write_bytes(b'authored-profile-v1')
        exe = self.root / 'train/ps2EntryRunner.exe'
        exe.write_bytes(b'authored-training-identity')
        (self.root / 'train-result.json').write_text(
            json.dumps({'sha256': helper.digest(exe)}), encoding='utf-8')
        (self.root / 'link-inputs.json').write_text('authored-manifest\n', encoding='utf-8')
        (self.root / 'optimized.rsp').write_text('authored-response\n', encoding='utf-8')
        self.manifest = {'tools': {'link': {'path': 'mock-link'},
                                   'pgomgr': {'path': 'mock-pgomgr'}},
                         'runtime_dlls': []}
        self.args = SimpleNamespace(output=self.root, stage='optimize')
        self.merge_inputs = []
        self.merge_paths = []
        self.link_count = 0
        self.fail_first = None
        self.addCleanup(patch.stopall)
        patch.object(helper, 'checked_manifest', return_value=self.manifest).start()
        patch.object(helper, 'run', side_effect=self.mock_run).start()

    def mock_run(self, arguments, root, manifest, log):
        log_path = root / log
        if arguments[0] == 'mock-pgomgr':
            work = Path(arguments[-1])
            self.assertNotEqual(work, self.training)
            self.assertEqual(work.read_bytes(), self.training_bytes)
            self.merge_inputs.append(work.read_bytes())
            self.merge_paths.append(work)
            profiles = b'|'.join(Path(path).read_bytes() for path in arguments[2:-1])
            work.write_bytes(work.read_bytes() + b'|merged:' + profiles)
            log_path.write_text('authored successful merge\n', encoding='utf-8')
            if len(self.merge_inputs) == 1:
                if self.fail_first == 'growing-pgc':
                    self.pgc.write_bytes(b'authored-profile-v2')
                elif self.fail_first == 'new-pgc':
                    (root / 'train/authored!2.pgc').write_bytes(b'authored-profile-extra')
                elif self.fail_first == 'partial-failure':
                    log_path.write_text('authored partial PGD update\n', encoding='utf-8')
                    raise ValueError('mock pgomgr exit 1 after partial update')
                elif self.fail_first == 'wrong-identity':
                    log_path.write_text('PG1052 authored mismatched profile\n', encoding='utf-8')
        else:
            self.assertEqual(arguments[0], 'mock-link')
            self.link_count += 1
            destination = root / 'optimized'
            # LINK may mutate its working PGD, but never the pinned merged input.
            working = destination / 'profile-work.pgd'
            working.write_bytes(working.read_bytes() + b'|link-bookkeeping')
            (destination / 'ps2EntryRunner.exe').write_bytes(b'authored-optimized-executable')
            (destination / 'ps2EntryRunner.pdb').write_bytes(b'authored-pdb')
            log_path.write_text('Finished generating code\n', encoding='utf-8')

    def assert_failed_attempt(self):
        self.assertEqual(self.training.read_bytes(), self.training_bytes)
        self.assertFalse((self.root / 'profile-inputs.json').exists())
        self.assertFalse((self.root / 'merge-profile.log').exists())
        self.assertFalse((self.root / 'optimized/profile-input.pgd').exists())
        self.assertFalse((self.root / '.pgo-stage.lock').exists())
        self.assertEqual(self.link_count, 0)
        self.assertTrue((self.root / 'merge-attempts/0001/merge.log').is_file())
        self.assertTrue(self.merge_paths[0].read_bytes().startswith(self.training_bytes + b'|merged:'))

    def assert_complete_once(self):
        self.assertEqual(self.training.read_bytes(), self.training_bytes)
        result = json.loads((self.root / 'optimized-result.json').read_text(encoding='utf-8'))
        inputs = json.loads((self.root / 'profile-inputs.json').read_text(encoding='utf-8'))
        merged = self.root / 'optimized/profile-input.pgd'
        self.assertEqual(merged.read_bytes().count(b'|merged:'), 1)
        self.assertEqual(helper.digest(merged), inputs['pgd_after_merge_sha256'])
        self.assertEqual(result['profile_input']['sha256'], helper.digest(merged))
        self.assertNotEqual(result['profile_output']['sha256'], result['profile_input']['sha256'])
        self.assertEqual(inputs['pgd_before_merge_sha256'], helper.digest(self.training))
        self.assertEqual(result['sha256'], helper.digest(self.root / 'optimized/ps2EntryRunner.exe'))
        helper.verify_records(result['preserved_files'])
        self.assertEqual(self.link_count, 1)

    def test_success_pins_unmerged_and_merged_inputs_separately(self):
        helper.relink(self.args)
        self.assert_complete_once()
        with self.assertRaisesRegex(ValueError, 'variant already exists'):
            helper.relink(self.args)
        self.assertEqual(len(self.merge_inputs), 1)

    def test_growing_pgc_retry_uses_clean_pgd(self):
        self.fail_first = 'growing-pgc'
        with self.assertRaisesRegex(ValueError, 'Pinned input changed'):
            helper.relink(self.args)
        self.assert_failed_attempt()
        failed = self.merge_paths[0].read_bytes()
        helper.relink(self.args)
        self.assert_complete_once()
        self.assertEqual(len(self.merge_inputs), 2)
        self.assertNotEqual(self.merge_paths[0], self.merge_paths[1])
        self.assertEqual(self.merge_paths[0].read_bytes(), failed)
        self.assertIn(b'authored-profile-v2', (self.root / 'optimized/profile-input.pgd').read_bytes())

    def test_new_pgc_retry_includes_full_set_once(self):
        self.fail_first = 'new-pgc'
        with self.assertRaisesRegex(ValueError, 'PGC set changed'):
            helper.relink(self.args)
        self.assert_failed_attempt()
        helper.relink(self.args)
        self.assert_complete_once()
        self.assertIn(b'authored-profile-extra', (self.root / 'optimized/profile-input.pgd').read_bytes())

    def test_partial_pgomgr_failure_retry_uses_clean_pgd(self):
        self.fail_first = 'partial-failure'
        with self.assertRaisesRegex(ValueError, 'partial update'):
            helper.relink(self.args)
        self.assert_failed_attempt()
        failed = self.merge_paths[0].read_bytes()
        helper.relink(self.args)
        self.assert_complete_once()
        self.assertEqual(self.merge_paths[0].read_bytes(), failed)
        self.assertEqual(self.merge_inputs, [self.training_bytes, self.training_bytes])

    def test_wrong_profile_identity_never_reaches_link(self):
        self.fail_first = 'wrong-identity'
        with self.assertRaisesRegex(ValueError, 'different PGD'):
            helper.relink(self.args)
        self.assert_failed_attempt()

    def test_legacy_unrecorded_merge_cannot_be_retried(self):
        (self.root / 'merge-profile.log').write_text('legacy incomplete merge\n', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'older merge attempt'):
            helper.relink(self.args)
        self.assertEqual(self.merge_inputs, [])
        self.assertEqual(self.training.read_bytes(), self.training_bytes)


if __name__ == '__main__':
    unittest.main()
