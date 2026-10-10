#!/usr/bin/env python3
"""Apply Holyland's ordered patch series without resetting a checkout.

Tools-only mode applies ps2xRecomp paths before the recompiler is built.
A verified phase record in Git's private directory makes overlapping runtime
patches repeatable without undoing local changes or reapplying earlier hunks.
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path


def git(checkout, *arguments):
    return subprocess.run(
        ['git', '-C', str(checkout), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding='utf-8',
        errors='replace',
        check=False,
    )


def checked_git(checkout, *arguments):
    result = git(checkout, *arguments)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout.strip()


def load_record(path):
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(data, dict) or not {'base', 'patches', 'files'} <= data.keys():
        raise RuntimeError(f'Invalid patch record: {path}')
    return data


def write_record(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.new')
    temporary.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    temporary.replace(path)


def affected_paths(checkout, patch, flags):
    output = checked_git(checkout, 'apply', '--numstat', '-z', *flags, str(patch))
    paths = set()
    for row in output.split('\0'):
        if not row:
            continue
        path = row.split('\t', 2)[-1]
        if not path or Path(path).is_absolute() or '..' in Path(path).parts:
            raise RuntimeError(f'Unsupported patch path in {patch.name}: {path}')
        paths.add(path)
    return paths


def file_hashes(checkout, paths):
    return {
        name: hashlib.sha256((checkout / name).read_bytes()).hexdigest()
        if (checkout / name).is_file() else None
        for name in sorted(paths)
    }


def verify_record(checkout, record, expected, patches, paths=None):
    if record['base'] != expected or record['patches'] != patches:
        raise RuntimeError(
            'The recorded phase differs from the pinned commit or current patch files. '
            'Use a fresh checkout when changing an already applied patch series.'
        )
    names = set(record['files']) if paths is None else paths
    actual = file_hashes(checkout, names)
    changed = [name for name in names if actual[name] != record['files'].get(name)]
    if changed:
        raise RuntimeError(
            'Patch-managed files changed after this phase was recorded; inspect them '
            'or use a fresh pinned checkout:\n' + '\n'.join(sorted(changed))
        )


def apply_series(checkout, patch_dir, base, tools_only):
    checkout = checkout.resolve(strict=True)
    patch_dir = patch_dir.resolve(strict=True)
    actual = checked_git(checkout, 'rev-parse', 'HEAD')
    expected = checked_git(checkout, 'rev-parse', f'{base}^{{commit}}')
    if actual != expected:
        raise RuntimeError(
            f'{checkout} must be at pinned commit {expected}; its HEAD is {actual}. '
            'Use a separate checkout for the pinned patch series.'
        )
    git_dir = Path(checked_git(checkout, 'rev-parse', '--absolute-git-dir'))
    record_dir = git_dir / 'kfiv-patches'
    scope = 'tools' if tools_only else 'full'
    record_path = record_dir / f'{scope}.json'
    record = load_record(record_path)
    other_record = load_record(record_dir / ('full.json' if tools_only else 'tools.json'))
    include = ['--include=ps2xRecomp/*'] if tools_only else []
    patches = []
    paths = set()
    for path in sorted(patch_dir.glob('*.patch')):
        affected = affected_paths(checkout, path, include)
        if not affected:
            continue
        paths.update(affected)
        patches.append({'name': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    if record is not None:
        verify_record(checkout, record, expected, patches)
        print(f'already applied {scope}: verified {len(patches)} patch(es) and {len(paths)} file(s)')
        return
    other_by_name = {}
    if other_record is not None:
        other_by_name = {entry['name']: entry['sha256'] for entry in other_record['patches']}
        if other_record['base'] != expected:
            raise RuntimeError('The other recorded phase uses a different pinned commit.')
        matching_paths = paths.intersection(other_record['files'])
        current_hashes = file_hashes(checkout, matching_paths)
        if any(current_hashes[name] != other_record['files'][name] for name in matching_paths):
            raise RuntimeError('Files from the other recorded phase changed; inspect the checkout.')
    for entry in patches:
        path = patch_dir / entry['name']
        flags = ['--ignore-space-change', *include]
        if other_by_name.get(entry['name']) == entry['sha256']:
            if tools_only:
                print(f'already applied tool hunks: {path.name}')
                continue
            # Tools-only mode may already have applied this patch's generator
            # hunks; apply its remaining runtime hunks without removing them.
            flags.append('--exclude=ps2xRecomp/*')
        forward = git(checkout, 'apply', '--check', *flags, str(path))
        if forward.returncode == 0:
            checked_git(checkout, 'apply', *flags, str(path))
            print(f'applied {scope}: {path.name}')
        elif git(checkout, 'apply', '--reverse', '--check', *flags, str(path)).returncode == 0:
            print(f'already applied {scope}: {path.name}')
        else:
            raise RuntimeError(
                f'{path.name} does not apply forward or reverse. Local edits were not reset; '
                'earlier patches in this invocation may already have applied.\n'
                + forward.stderr.strip()
                + '\nFor a manually patched or partially applied full series, use a fresh checkout.'
            )
    write_record(record_path, {
        'base': expected,
        'patches': patches,
        'files': file_hashes(checkout, paths),
    })
    print(f'{scope} patch series ready: {len(patches)} patch(es) at {actual[:12]}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('checkout', type=Path)
    parser.add_argument('--patch-dir', type=Path,
                        default=Path(__file__).resolve().parent.parent / 'patches')
    parser.add_argument('--base', default='c5a9d02')
    parser.add_argument('--tools-only', action='store_true')
    args = parser.parse_args()
    try:
        apply_series(args.checkout, args.patch_dir, args.base, args.tools_only)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f'error: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
