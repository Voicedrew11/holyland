"""Exercise patch-phase safety using private synthetic Git repositories."""

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

HELPER = Path(__file__).resolve().parents[1] / 'scripts/apply-patches.py'
spec = importlib.util.spec_from_file_location('apply_patches', HELPER)
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


class PatchSeriesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='KFIV patch tests ')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "checkout with spaces and apostrophe's"
        self.repo.mkdir()
        self.patches = self.root / 'patches'
        self.patches.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Synthetic fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.git('config', 'core.autocrlf', 'true')
        self.git('config', 'core.safecrlf', 'false')
        for path in ['ps2xRuntime/value.txt', 'ps2xRecomp/value.txt']:
            target = self.repo / path
            target.parent.mkdir(exist_ok=True)
            target.write_bytes(b'first\r\nbase\r\nlast\r\n')
        (self.repo / 'unrelated.txt').write_bytes(b'original\r\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'Synthetic base')
        self.base = self.git('rev-parse', 'HEAD').strip()
        self.make_patch('0001-runtime.patch', 'ps2xRuntime/value.txt', 'base', 'first-fix')
        self.make_patch('0002-runtime.patch', 'ps2xRuntime/value.txt', 'first-fix', 'final-fix')
        self.make_patch('0022-tools.patch', 'ps2xRecomp/value.txt', 'base', 'tool-fix')

    def git(self, *args):
        result = subprocess.run(['git', '-C', str(self.repo), *args],
                                text=True, capture_output=True, check=True)
        return result.stdout

    def make_patch(self, name, path, old, new):
        text = (f'diff --git a/{path} b/{path}\n'
                f'--- a/{path}\n+++ b/{path}\n'
                f'@@ -1,3 +1,3 @@\n first\n-{old}\n+{new}\n last\n')
        (self.patches / name).write_text(text, encoding='utf-8')

    def apply(self, tools=False):
        patcher.apply_series(self.repo, self.patches, self.base, tools)

    def test_tools_twice_full_after_tools_full_twice_and_tools_after_full(self):
        self.apply(True)
        self.apply(True)
        self.assertIn('base', (self.repo / 'ps2xRuntime/value.txt').read_text())
        self.apply()
        self.apply()
        self.apply(True)
        self.assertIn('final-fix', (self.repo / 'ps2xRuntime/value.txt').read_text())
        self.assertIn('tool-fix', (self.repo / 'ps2xRecomp/value.txt').read_text())

    def test_fresh_full_preserves_unrelated_edits_and_index(self):
        before_index = self.git('ls-files', '--stage')
        unrelated = self.repo / 'unrelated.txt'
        unrelated.write_bytes(b'private local edit\r\n')
        self.apply()
        self.apply()
        self.assertEqual(unrelated.read_bytes(), b'private local edit\r\n')
        self.assertEqual(self.git('ls-files', '--stage'), before_index)

    def test_changed_patch_file_is_rejected(self):
        self.apply()
        path = self.patches / '0001-runtime.patch'
        path.write_bytes(path.read_bytes() + b'\n')
        with self.assertRaisesRegex(RuntimeError, 'recorded phase differs'):
            self.apply()

    def test_changed_record_is_rejected(self):
        self.apply()
        path = self.repo / '.git/kfiv-patches/full.json'
        record = json.loads(path.read_text())
        record['base'] = 'wrong-base'
        path.write_text(json.dumps(record), encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'recorded phase differs'):
            self.apply()

    def test_changed_managed_file_is_rejected(self):
        self.apply()
        target = self.repo / 'ps2xRuntime/value.txt'
        target.write_bytes(b'local implementation edit\r\n')
        with self.assertRaisesRegex(RuntimeError, 'Patch-managed files changed'):
            self.apply()
        self.assertEqual(target.read_bytes(), b'local implementation edit\r\n')

    def test_wrong_head_is_rejected(self):
        (self.repo / 'new.txt').write_text('new commit', encoding='utf-8')
        self.git('add', 'new.txt')
        self.git('commit', '-qm', 'Wrong HEAD fixture')
        with self.assertRaisesRegex(RuntimeError, 'must be at pinned commit'):
            self.apply()
        self.assertIn('base', (self.repo / 'ps2xRuntime/value.txt').read_text())

    def test_failed_phase_does_not_write_completion_record(self):
        self.make_patch('0002-runtime.patch', 'ps2xRuntime/value.txt', 'unmatched', 'final-fix')
        with self.assertRaisesRegex(RuntimeError, 'earlier patches'):
            self.apply()
        self.assertFalse((self.repo / '.git/kfiv-patches/full.json').exists())
        self.assertIn('first-fix', (self.repo / 'ps2xRuntime/value.txt').read_text())


if __name__ == '__main__':
    unittest.main()
