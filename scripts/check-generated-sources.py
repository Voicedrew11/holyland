#!/usr/bin/env python3
"""Reject KFIV generated sources that predate the EE square-root fix.

This is a guard for a known stale-output failure, not a provenance certificate
or proof that all generated functions are complete/correct. Rebuild the patched
recompiler and regenerate the whole game after any generator patch changes.
"""

import argparse
import re
from pathlib import Path


LEGACY = re.compile(
    rb'FPU_SQRT_S\s*\(\s*ctx->f\s*\['
    rb'|1\.0f\s*/\s*sqrtf\s*\(\s*ctx->f\s*\[')
PATCHED = re.compile(rb'FPU_SQRT_S\s*\(\s*(?:input|denominator)\s*\)')


def validate(source_dir, header_dir=None):
    source_dir = Path(source_dir)
    header_dir = Path(header_dir) if header_dir is not None else source_dir
    for name in ('ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h'):
        if not (header_dir / name).is_file():
            raise ValueError(f'Missing generated header: {header_dir / name}')
    if not (source_dir / 'register_functions.cpp').is_file():
        raise ValueError(f'Missing generated function table in {source_dir}')
    sources = sorted(source_dir.glob('*.cpp'))
    patched_count = 0
    for source in sources:
        content = source.read_bytes()
        if LEGACY.search(content):
            raise ValueError(
                f'Stale EE SQRT/RSQRT translation in {source}. '
                'Rebuild the recompiler with patch 0029 and regenerate the '
                'complete output before staging or building the runner.')
        patched_count += len(PATCHED.findall(content))
    if not patched_count:
        raise ValueError(
            f'No patched EE square-root translations in {source_dir}; '
            'regenerate and stage the complete KFIV output first.')
    return len(sources), patched_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--output', type=Path, help='Private generated output directory')
    group.add_argument('--checkout', type=Path, help='Runtime checkout to check before building')
    args = parser.parse_args()
    try:
        if args.checkout is not None:
            count, roots = validate(args.checkout / 'ps2xRuntime/src/runner',
                                    args.checkout / 'ps2xRuntime/include')
        else:
            count, roots = validate(args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f'error: {error}\n')
    print(f'Generated-source guard: {count} sources, {roots} patched square-root sites; no legacy sites')


if __name__ == '__main__':
    main()
