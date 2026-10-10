#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze an MSVC runner link and relink private PGO variants without launching it."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def inside(path, directory):
    return path == directory or directory in path.parents


def split_windows(command):
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int))
    shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.LocalFree.argtypes = (wintypes.HLOCAL,)
    kernel.LocalFree.restype = wintypes.HLOCAL
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW('link.exe ' + command, ctypes.byref(count))
    if not pointer:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return [pointer[index] for index in range(1, count.value)]
    finally:
        kernel.LocalFree(pointer)


def quote(argument):
    if '"' in argument or argument.endswith('\\') or '\n' in argument or '\r' in argument:
        raise ValueError(f'Unsupported response-file token: {argument!r}')
    return '"' + argument + '"'


def resolve_input(argument, build, libraries):
    path = Path(argument)
    locations = [path] if path.is_absolute() else [build / path]
    if path.name == argument and path.suffix.lower() == '.lib':
        locations.extend(directory / path for directory in libraries)
    for location in locations:
        if location.is_file():
            return location.resolve()
    raise ValueError(f'Cannot resolve link input {argument!r}; use the matching VS developer shell')


def prepare(args):
    root, build, source = (path.resolve() for path in
                           (args.output, args.runner_build_dir, args.source_dir))
    repository = Path(__file__).resolve().parents[2]
    for protected in (repository, build, source):
        if inside(root, protected) or inside(protected, root):
            raise ValueError('PGO output must be outside the repository, source checkout and build tree')
    if root.exists():
        raise ValueError('Choose a new unused PGO output directory; frozen inputs are never overwritten')
    msvc, sdk = args.msvc_bin.resolve(), args.sdk_bin.resolve()
    tools = {'link': msvc / 'link.exe', 'pgomgr': msvc / 'pgomgr.exe',
             'pgort': msvc / 'pgort140.dll', 'rc': sdk / 'rc.exe'}
    if not all(path.is_file() for path in tools.values()):
        raise ValueError('MSVC and Windows SDK directories must supply link, pgomgr, pgort140 and rc')
    libraries = [Path(item).resolve() for item in os.environ.get('LIB', '').split(';') if item]
    if not libraries:
        raise ValueError('LIB is unset; run preparation from the matching x64 VS developer shell')
    config = args.configuration
    tlog = build / f'ps2EntryRunner.dir/{config}/ps2EntryRunner.tlog/link.command.1.tlog'
    runner = build / config / 'ps2EntryRunner.exe'
    if not runner.is_file() or not runner.with_suffix('.pdb').is_file():
        raise ValueError('The selected final runner EXE/PDB must already be built')
    lines = tlog.read_text(encoding='utf-16').splitlines()
    arguments = split_windows(' '.join(line for line in lines if line and not line.startswith('^')))
    if any(argument.upper().startswith(('/GENPROFILE', '/FASTGENPROFILE', '/USEPROFILE'))
           for argument in arguments):
        raise ValueError('Supply the ordinary final runner link, not an already profiled link')
    # This helper snapshots the ordinary KFIV runner graph, rather than all
    # possible LINK features. Refuse embedded inputs and extra output paths
    # that would otherwise escape the frozen-input/private-output contract.
    unsupported = ('/DEF:', '/WHOLEARCHIVE:', '/ORDER:', '/MANIFESTINPUT:', '/STUB:',
                   '/ASSEMBLYRESOURCE:', '/ASSEMBLYMODULE:', '/SOURCELINK:', '/NATVIS:',
                   '/TLBOUT:', '/WINMDFILE:', '/PGD:', '/LINKREPRO')
    for argument in arguments:
        if argument.upper().startswith(unsupported):
            raise ValueError(f'Unsupported file-bearing LINK option: {argument}')
    inputs = []
    replacements = {}
    # Resolve every explicit object/archive before writing any output.
    for index, argument in enumerate(arguments):
        if argument.startswith('/'):
            continue
        path = resolve_input(argument, build, libraries)
        inputs.append({'source': str(path), 'frozen': str(root / 'inputs' /
                       f'{len(inputs):04d}-{path.name.lower()}'),
                       'bytes': path.stat().st_size, 'sha256': digest(path)})
        replacements[index] = inputs[-1]['frozen']
    if not inputs or not any(argument.upper().startswith('/LTCG') for argument in arguments):
        raise ValueError('Expected an ordinary LTCG runner link with explicit object/archive inputs')
    dll_directory = (args.runtime_dll_dir or runner.parent).resolve()
    dlls = [{'source': str(path), 'name': path.name, 'sha256': digest(path)}
            for path in sorted(dll_directory.glob('*.dll'))]
    root.mkdir()
    (root / 'inputs').mkdir()
    for item in inputs:
        shutil.copyfile(item['source'], item['frozen'])
        if digest(Path(item['frozen'])) != item['sha256'] or digest(Path(item['source'])) != item['sha256']:
            raise ValueError('A link input changed during snapshot; discard this incomplete private snapshot')
    (root / 'original').mkdir()
    for path in (runner, runner.with_suffix('.pdb'), tlog):
        shutil.copyfile(path, root / 'original' / path.name)
    for mode in ('baseline', 'train', 'optimized'):
        output = root / mode
        output.mkdir()
        rewritten = []
        for index, argument in enumerate(arguments):
            upper = argument.upper()
            destinations = {'/OUT:': 'ps2EntryRunner.exe', '/PDB:': 'ps2EntryRunner.pdb',
                            '/LTCGOUT:': 'ps2EntryRunner.iobj', '/IMPLIB:': 'ps2EntryRunner.lib',
                            '/ILK:': 'ps2EntryRunner.ilk', '/MAP:': 'ps2EntryRunner.map',
                            '/PDBSTRIPPED:': 'ps2EntryRunner-stripped.pdb',
                            '/MANIFESTFILE:': 'ps2EntryRunner.manifest'}
            for prefix, name in destinations.items():
                if upper.startswith(prefix):
                    argument = prefix + str(output / name)
                    break
            else:
                if upper.startswith('/INCREMENTAL'):
                    argument = '/INCREMENTAL:NO'
                elif upper == '/MAP':
                    argument = '/MAP:' + str(output / 'ps2EntryRunner.map')
                elif upper.startswith('/LTCG'):
                    argument = '/LTCG'
                elif index in replacements:
                    argument = replacements[index]
            rewritten.append(argument)
        if mode == 'train':
            rewritten.append('/GENPROFILE:PGD=' + str(root / 'training.pgd'))
        elif mode == 'optimized':
            rewritten.append('/USEPROFILE:PGD=' + str(output / 'profile-work.pgd'))
        (root / (mode + '.rsp')).write_text('\n'.join(map(quote, rewritten)) + '\n', encoding='utf-8')
        for item in dlls:
            shutil.copyfile(item['source'], output / item['name'])
            if digest(output / item['name']) != item['sha256']:
                raise ValueError('A runtime DLL changed during snapshot')
    shutil.copyfile(tools['pgort'], root / 'train/pgort140.dll')
    manifest = {'schema': 1, 'configuration': config, 'source_dir': str(source),
                'runner_build_dir': str(build), 'runner_sha256': digest(runner),
                'inputs': inputs, 'runtime_dlls': dlls, 'library_environment': os.environ['LIB'],
                'msvc_bin': str(msvc), 'sdk_bin': str(sdk),
                'responses': {mode: digest(root / (mode + '.rsp'))
                              for mode in ('baseline', 'train', 'optimized')},
                'tools': {name: {'path': str(path), 'sha256': digest(path)}
                          for name, path in tools.items()}}
    (root / 'link-inputs.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Frozen {len(inputs)} inputs. No linker or game was launched. Private output: {root}')


def checked_manifest(root):
    manifest = json.loads((root / 'link-inputs.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 1:
        raise ValueError('Unsupported PGO manifest schema')
    for item in manifest['inputs']:
        path = Path(item['frozen']).resolve()
        if not inside(path, root / 'inputs') or digest(path) != item['sha256']:
            raise ValueError(f'Frozen link input changed: {path}')
    for item in manifest['tools'].values():
        if digest(Path(item['path'])) != item['sha256']:
            raise ValueError('MSVC/SDK tool changed since preparation; create a fresh snapshot')
    for mode, expected in manifest['responses'].items():
        if mode not in ('baseline', 'train', 'optimized') or digest(root / (mode + '.rsp')) != expected:
            raise ValueError('A frozen link response changed; create a fresh snapshot')
    for item in manifest['runtime_dlls']:
        if Path(item['name']).name != item['name']:
            raise ValueError('Invalid runtime DLL name in manifest')
        for mode in ('baseline', 'train', 'optimized'):
            if digest(root / mode / item['name']) != item['sha256']:
                raise ValueError('A copied runtime DLL changed since preparation')
    if digest(root / 'train/pgort140.dll') != manifest['tools']['pgort']['sha256']:
        raise ValueError('The copied profiling runtime changed since preparation')
    return manifest


def run(args, root, manifest, log):
    environment = os.environ.copy()
    environment['LIB'] = manifest['library_environment']
    environment['PATH'] = os.pathsep.join((manifest['msvc_bin'], manifest['sdk_bin'],
                                          environment.get('PATH', '')))
    with (root / log).open('w', encoding='utf-8') as output:
        result = subprocess.run([str(item) for item in args], cwd=root, env=environment,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f'{log}: exit {result.returncode}; inspect the private log')


def file_record(path):
    return {'path': str(path), 'sha256': digest(path), 'bytes': path.stat().st_size}


def verify_records(records):
    for item in records:
        path = Path(item['path'])
        if path.stat().st_size != item['bytes'] or digest(path) != item['sha256']:
            raise ValueError(f'Pinned input changed: {path}')


def checked_profiles(root, records):
    actual = {str(path.resolve()) for path in (root / 'train').glob('*.pgc')}
    expected = {str(Path(item['path']).resolve()) for item in records}
    if actual != expected:
        raise ValueError('The training PGC set changed; quit training before optimization')
    verify_records(records)


def copy_checked(source, destination, expected):
    shutil.copyfile(source, destination)
    if digest(source) != expected or digest(destination) != expected:
        raise ValueError('The profile changed while making an isolated copy')


def finish_profile_link(root, manifest, destination, response, profile_inputs,
                        preserved, stage):
    # LINK /USEPROFILE can update PGD bookkeeping. Only this exclusive
    # writable copy is passed to LINK; the pinned input remains untouched.
    profile_input = destination / 'profile-input.pgd'
    profile_work = destination / 'profile-work.pgd'
    input_hash = digest(profile_input)
    copy_checked(profile_input, profile_work, input_hash)
    manifest_hash = digest(root / 'link-inputs.json')
    stage_inputs = [file_record(response), file_record(destination / 'profile-inputs.json')]
    stage_inputs.extend(file_record(destination / item['name']) for item in manifest['runtime_dlls'])
    checked_manifest(root)
    checked_profiles(root, profile_inputs['profiles'])
    verify_records(preserved)
    run([manifest['tools']['link']['path'], '@' + str(response)], root, manifest,
        str((destination / 'link.log').relative_to(root)))
    if digest(root / 'link-inputs.json') != manifest_hash:
        raise ValueError('The frozen link manifest changed during optimization')
    checked_manifest(root)
    checked_profiles(root, profile_inputs['profiles'])
    verify_records(preserved)
    verify_records(stage_inputs)
    if digest(profile_input) != input_hash:
        raise ValueError('The immutable profile input changed during linking')
    exe = destination / 'ps2EntryRunner.exe'
    result = {'schema': 2, 'executable': str(exe), 'sha256': digest(exe),
              'pdb': file_record(exe.with_suffix('.pdb')), 'stage': stage,
              'link_inputs_sha256': digest(root / 'link-inputs.json'),
              'response': file_record(response),
              'profile_input': file_record(profile_input),
              'profile_output': file_record(profile_work),
              'profile_inputs': file_record(destination / 'profile-inputs.json'),
              'link_log': file_record(destination / 'link.log'),
              'preserved_files': preserved, 'stage_inputs': stage_inputs}
    (root / 'optimized-result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(f'{stage} complete: {exe}\nNo game was launched.')


def recover_optimized(args, root, manifest):
    if (root / 'optimized-result.json').exists():
        raise ValueError('An optimized result already exists; never overwrite completed evidence')
    prior = root / 'optimized'
    if not all((prior / name).is_file() for name in ('ps2EntryRunner.exe', 'ps2EntryRunner.pdb')):
        raise ValueError('Recovery requires the preserved outputs of the earlier optimized link')
    log = root / 'optimized-link.log'
    if not log.is_file() or 'Finished generating code' not in log.read_text(encoding='utf-8'):
        raise ValueError('The earlier optimized link has no successful code-generation evidence')
    pgd = root / 'training.pgd'
    expected = args.expected_pgd_sha256.lower()
    if len(expected) != 64 or any(c not in '0123456789abcdef' for c in expected) or digest(pgd) != expected:
        raise ValueError('Recovery PGD does not match the explicitly reviewed hash')
    profiles = json.loads((root / 'profile-inputs.json').read_text(encoding='utf-8'))
    checked_profiles(root, profiles['profiles'])
    trained = json.loads((root / 'train-result.json').read_text(encoding='utf-8'))
    if digest(root / 'train/ps2EntryRunner.exe') != trained['sha256'] or trained['sha256'] != profiles['training_executable_sha256']:
        raise ValueError('The training executable identity changed')
    preserved_paths = [pgd, root / 'profile-inputs.json', root / 'merge-profile.log', log,
                       root / 'train/ps2EntryRunner.exe', root / 'train-result.json']
    preserved_paths.extend(path for path in sorted(prior.iterdir()) if path.is_file())
    preserved = [file_record(path) for path in preserved_paths]
    destination = root / 'optimized-recovered'
    if destination.exists():
        raise ValueError('Recovery output already exists; preserve it and use a fresh snapshot')
    destination.mkdir()
    copy_checked(pgd, destination / 'profile-input.pgd', expected)
    # Do not merge again: the preserved PGD already contains the exact PGC
    # merge, followed by the original successful LINK bookkeeping update.
    recovery_inputs = dict(profiles, schema=2, recovery=True,
                           recovered_pgd_sha256=expected,
                           original_profile_record=file_record(root / 'profile-inputs.json'))
    (destination / 'profile-inputs.json').write_text(json.dumps(recovery_inputs, indent=2) + '\n', encoding='utf-8')
    arguments = split_windows(' '.join((root / 'optimized.rsp').read_text(encoding='utf-8').splitlines()))
    rewritten = []
    for argument in arguments:
        upper = argument.upper()
        if upper.startswith('/USEPROFILE:PGD='):
            argument = '/USEPROFILE:PGD=' + str(destination / 'profile-work.pgd')
        else:
            for prefix in ('/OUT:', '/PDB:', '/LTCGOUT:', '/IMPLIB:', '/ILK:', '/MAP:', '/PDBSTRIPPED:', '/MANIFESTFILE:'):
                if upper.startswith(prefix):
                    old = Path(argument[len(prefix):]).resolve()
                    if not inside(old, prior):
                        raise ValueError('An original output path escapes the private optimized stage')
                    argument = prefix + str(destination / old.name)
                    break
        rewritten.append(argument)
    response = destination / 'optimized.rsp'
    response.write_text('\n'.join(map(quote, rewritten)) + '\n', encoding='utf-8')
    for item in manifest['runtime_dlls']:
        shutil.copyfile(prior / item['name'], destination / item['name'])
        if digest(destination / item['name']) != item['sha256']:
            raise ValueError('A recovery runtime DLL changed during copying')
    finish_profile_link(root, manifest, destination, response, recovery_inputs, preserved, args.stage)


def relink_unlocked(args):
    root = args.output.resolve()
    manifest = checked_manifest(root)
    link = manifest['tools']['link']['path']
    if args.stage == 'recover-optimized':
        recover_optimized(args, root, manifest)
        return
    mode = {'link-baseline': 'baseline', 'link-training': 'train', 'optimize': 'optimized'}[args.stage]
    if (root / mode / 'ps2EntryRunner.exe').exists():
        raise ValueError('This variant already exists; create a fresh snapshot instead of overwriting it')
    if args.stage == 'link-training' and (root / 'training.pgd').exists():
        raise ValueError('A training PGD already exists; do not mix different instrumented link identities')
    if args.stage == 'optimize':
        if (root / 'profile-inputs.json').exists():
            raise ValueError('A merge record already exists; do not merge the same PGC files twice')
        profiles = sorted((root / 'train').glob('*.pgc'))
        if not profiles or not (root / 'training.pgd').is_file():
            raise ValueError('Run the private training EXE and quit normally before optimization')
        trained = json.loads((root / 'train-result.json').read_text(encoding='utf-8'))
        if digest(root / 'train/ps2EntryRunner.exe') != trained['sha256']:
            raise ValueError('The instrumented training executable changed')
        profile_inputs = {'schema': 1, 'training_executable_sha256': trained['sha256'],
                          'pgd_before_merge_sha256': digest(root / 'training.pgd'),
                          'profiles': [{'path': str(path), 'sha256': digest(path),
                                        'bytes': path.stat().st_size} for path in profiles]}
        run([manifest['tools']['pgomgr']['path'], '/merge', *profiles, root / 'training.pgd'],
            root, manifest, 'merge-profile.log')
        if 'PG1052' in (root / 'merge-profile.log').read_text(encoding='utf-8'):
            raise ValueError('A PGC belongs to a different PGD; do not optimize with mixed training identities')
        for item in profile_inputs['profiles']:
            if digest(Path(item['path'])) != item['sha256']:
                raise ValueError('A training PGC changed during merge; quit the game before optimizing')
        profile_inputs['pgd_after_merge_sha256'] = digest(root / 'training.pgd')
        (root / 'profile-inputs.json').write_text(json.dumps(profile_inputs, indent=2) + '\n', encoding='utf-8')
        destination = root / 'optimized'
        copy_checked(root / 'training.pgd', destination / 'profile-input.pgd', profile_inputs['pgd_after_merge_sha256'])
        (destination / 'profile-inputs.json').write_text(json.dumps(profile_inputs, indent=2) + '\n', encoding='utf-8')
        finish_profile_link(root, manifest, destination, root / 'optimized.rsp', profile_inputs,
                            [file_record(root / 'training.pgd'), file_record(root / 'profile-inputs.json'),
                             file_record(root / 'train/ps2EntryRunner.exe')], args.stage)
        return
    run([link, '@' + str(root / (mode + '.rsp'))], root, manifest, mode + '-link.log')
    exe = root / mode / 'ps2EntryRunner.exe'
    (root / (mode + '-result.json')).write_text(json.dumps({'executable': str(exe),
        'sha256': digest(exe), 'stage': args.stage}, indent=2) + '\n', encoding='utf-8')
    print(f'{args.stage} complete: {exe}\nNo game was launched.')


def relink(args):
    root = args.output.resolve()
    lock = root / '.pgo-stage.lock'
    # Refuse concurrent helper stages. A process crash deliberately leaves
    # this marker for inspection; never infer that another owner is idle.
    with lock.open('x', encoding='utf-8') as handle:
        handle.write(str(os.getpid()) + '\n')
    try:
        relink_unlocked(args)
    finally:
        lock.unlink()


def main():
    if os.name != 'nt':
        raise ValueError('This workflow requires native Windows MSVC')
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='stage', required=True)
    prepare_parser = commands.add_parser('prepare')
    prepare_parser.add_argument('--runner-build-dir', type=Path, required=True)
    prepare_parser.add_argument('--source-dir', type=Path, required=True)
    prepare_parser.add_argument('--configuration', default='RelWithDebInfo')
    prepare_parser.add_argument('--msvc-bin', type=Path, required=True)
    prepare_parser.add_argument('--sdk-bin', type=Path, required=True)
    prepare_parser.add_argument('--runtime-dll-dir', type=Path)
    prepare_parser.add_argument('--output', type=Path, required=True)
    for stage in ('link-baseline', 'link-training', 'optimize'):
        commands.add_parser(stage).add_argument('--output', type=Path, required=True)
    recovery = commands.add_parser('recover-optimized')
    recovery.add_argument('--output', type=Path, required=True)
    recovery.add_argument('--expected-pgd-sha256', required=True)
    args = parser.parse_args()
    if args.stage == 'prepare':
        prepare(args)
    else:
        relink(args)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, UnicodeError, KeyError) as error:
        print(f'PGO preparation failed: {error}', file=sys.stderr)
        sys.exit(1)
