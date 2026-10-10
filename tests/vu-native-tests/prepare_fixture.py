# SPDX-License-Identifier: GPL-3.0-or-later
"""Derive an authored native verifier in an external build tree.

Canonical runtime and fixture sources are read-only. Nothing from a retail
disc, executable, recording or generated catalog is bundled in this package.
"""
import argparse
from pathlib import Path
import sys
sys.dont_write_bytecode = True
from authored_program import generate

BASE_FILES = (
    'CMakeLists.txt', 'prepare_sources.py', 'reference39-hashes.json',
    'verifier.cpp', 'metadata_cases.inc', 'pipeline_cases.inc',
    'unrelated_stubs.cpp',
)


def inside(path, directory):
    return path == directory or directory in path.parents


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f'Expected one verifier/CMake anchor: {before[:80]!r}')
    return text.replace(before, after, 1)


def write_changed(path, text):
    data = text.encode('utf-8')
    if not path.is_file() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


def prepare(source, destination, scratch, build_root, candidate, reference):
    source, destination, scratch, build_root, candidate, reference = (
        path.resolve() for path in
        (source, destination, scratch, build_root, candidate, reference))
    here = Path(__file__).resolve().parent
    if candidate == reference:
        raise ValueError('Candidate and patch-39 reference must be separate runtime directories')
    repository = next((parent for parent in here.parents
        if (parent / '.git').exists() or
           ((parent / 'AGENTS.md').is_file() and (parent / 'tests').is_dir())), here)
    for protected in (repository, source, here, candidate, reference):
        if inside(build_root, protected) or inside(protected, build_root):
            raise ValueError('Build scratch must be separate from all canonical source trees')
    if not inside(destination, build_root) or not inside(scratch, build_root):
        raise ValueError('Derived fixtures and authored inputs must remain in the supplied build tree')
    if inside(destination, scratch) or inside(scratch, destination):
        raise ValueError('Derived source and generated authored input directories must be separate')

    # Read and validate every adaptation before creating output. Keep copying
    # limited to the source-only files the differential fixture actually uses.
    derived = {name: (source / name).read_text(encoding='utf-8') for name in BASE_FILES}
    derived['authored_native_cases.inc'] = (here / 'authored_native_cases.inc').read_text(encoding='utf-8')
    derived['check_native_control.py'] = (here / 'check_native_control.py').read_text(encoding='utf-8')
    derived['verifier.cpp'] = replace_once(derived['verifier.cpp'],
        '#include "metadata_cases.inc"',
        '#include "metadata_cases.inc"\n#include "authored_native_cases.inc"')
    derived['verifier.cpp'] = replace_once(derived['verifier.cpp'],
        '            else if (arg == "--metadata")',
        '            else if (arg == "--authored-native") { authoredNativeSynthetic(); '
        'if (!control.empty()) throw std::runtime_error("negative control was not detected"); '
        'std::cout << "PASS authored-native checks=" << checks << " calls=" << calls << \'\\n\'; return 0; }\n'
        '            else if (arg == "--metadata")')
    cmake = replace_once(derived['CMakeLists.txt'],
        'target_compile_features(kfiv_vu_performance_verify PRIVATE cxx_std_20)', '''
target_compile_features(kfiv_vu_performance_verify PRIVATE cxx_std_20)
if(NOT EXISTS "${VU_NATIVE_CATALOG}" OR NOT EXISTS "${VU_AUTHORED_CATALOG_DIR}/authored_catalog.inc")
    message(FATAL_ERROR "The outer native fixture must generate the authored catalog and header")
endif()
set_property(SOURCE "${generated}/candidate/src/lib/vu/ps2_vu1_core.cpp" APPEND PROPERTY
    COMPILE_DEFINITIONS "PS2X_VU1_NATIVE_CATALOG=\\"${VU_NATIVE_CATALOG}\\"")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VU_NATIVE_CATALOG}" "${VU_AUTHORED_CATALOG_DIR}/authored_catalog.inc")
target_compile_definitions(kfiv_vu_performance_verify PRIVATE VU_SPECIALIZATION_VERIFY_COUNTER=1)
target_include_directories(kfiv_vu_performance_verify PRIVATE "${VU_AUTHORED_CATALOG_DIR}")
''')
    if cmake.count('enable_testing()') != 1:
        raise ValueError('Expected one base CTest registration boundary')
    # These tests specifically execute the compiled authored catalog. The base
    # package retains its independent generic synthetic/metadata/replay suite.
    cmake = cmake[:cmake.index('enable_testing()')] + '''enable_testing()
add_test(NAME vu_native_authored_exact COMMAND kfiv_vu_performance_verify --authored-native)
set_tests_properties(vu_native_authored_exact PROPERTIES TIMEOUT 180)
foreach(kind IN ITEMS registers flags cycles memory packet)
    add_test(NAME "vu_native_${kind}_negative_control"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/check_native_control.py"
        --executable "$<TARGET_FILE:kfiv_vu_performance_verify>" --category "${kind}")
endforeach()
get_property(vu_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${vu_registered_tests} PROPERTIES ENVIRONMENT "PS2X_VU1_LIFT=0")
'''
    derived['CMakeLists.txt'] = cmake
    generate(scratch)
    for name, content in derived.items():
        write_changed(destination / name, content)
    print(f'Prepared authored native verifier sources in external build scratch: {destination}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ('source-fixture', 'destination', 'scratch', 'build-root',
                   'candidate-runtime', 'reference-runtime'):
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source_fixture, args.destination, args.scratch, args.build_root,
            args.candidate_runtime, args.reference_runtime)


if __name__ == '__main__':
    main()
