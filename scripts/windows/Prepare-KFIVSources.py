#!/usr/bin/env python3
"""Prepare private game configuration and copy generated sources by content."""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


def configure(game, repository):
    game = game.resolve(strict=True)
    elf = game / 'SLUS_203.18'
    expected = (repository / 'kfiv/SLUS_203.18.sha256').read_text().split()[0]
    actual = hashlib.sha256(elf.read_bytes()).hexdigest()
    if actual.lower() != expected.lower():
        raise ValueError(f'Unsupported boot ELF: expected {expected}, found {actual}')
    output = game / 'output'
    output.mkdir(exist_ok=True)
    mapping = repository / 'kfiv/SLUS_203.18.functions.csv'
    text = (repository / 'kfiv/config.toml').read_text(encoding='utf-8')
    for key, path in [('input', elf), ('output', output), ('ghidra_output', mapping)]:
        replacement = f'{key} = {json.dumps(path.as_posix())}'
        text, count = re.subn(rf'(?m)^{key}\s*=.*$', lambda _: replacement, text)
        if count != 1:
            raise ValueError(f'Expected one {key} setting in kfiv/config.toml')
    config = game / 'config.toml'
    config.write_text(text, encoding='utf-8')
    print(f'ELF verified; configuration written to {config}')


def stage(game, checkout):
    output = game.resolve(strict=True) / 'output'
    checkout = checkout.resolve(strict=True)
    headers = sorted(output.glob('*.h'))
    sources = sorted(output.glob('*.cpp'))
    if not headers or not sources or not (output / 'register_functions.cpp').is_file():
        raise ValueError('Generate the complete game output with the patched recompiler first')
    destinations = [(headers, checkout / 'ps2xRuntime/include'),
                    (sources, checkout / 'ps2xRuntime/src/runner')]
    copied = 0
    for files, directory in destinations:
        if not directory.is_dir():
            raise ValueError(f'Invalid runtime checkout: {directory}')
        for source in files:
            target = directory / source.name
            if not target.is_file() or source.read_bytes() != target.read_bytes():
                shutil.copyfile(source, target)
                copied += 1
    print(f'Staged {len(headers)} header(s), {len(sources)} source(s); copied {copied} changed file(s)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['config', 'stage'])
    parser.add_argument('--game-dir', required=True, type=Path)
    parser.add_argument('--checkout', type=Path, help='Patched runtime checkout, required for stage')
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    try:
        if args.action == 'config':
            configure(args.game_dir, repository)
        elif args.checkout is None:
            parser.error('--checkout is required for stage')
        else:
            stage(args.game_dir, args.checkout)
    except (OSError, ValueError) as error:
        parser.exit(1, f'error: {error}\n')


if __name__ == '__main__':
    main()
