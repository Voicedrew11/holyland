# SPDX-License-Identifier: GPL-3.0-or-later
"""Require a native-issued, category-specific differential failure."""
import argparse
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--executable', required=True)
parser.add_argument('--category', choices=('registers', 'flags', 'cycles', 'memory', 'packet'), required=True)
args = parser.parse_args()
result = subprocess.run([args.executable, '--host-jobs', '--control=' + args.category, '--authored-native'],
                        capture_output=True, text=True, timeout=120)
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
expected = re.compile(r'^MISMATCH call=\d+ category=' + args.category + r'$', re.MULTILINE)
issues = re.search(r'^PRIVATE-SPECIALIZATION issued=(\d+)$', result.stdout, re.MULTILINE)
if result.returncode != 1 or not expected.search(result.stderr) or not issues or int(issues[1]) == 0:
    parser.exit(1, f'Expected exit 1, native-issued > 0 and the {args.category} differential mismatch; '
                   f'got exit {result.returncode}.\n')
print(f'PASS detected {args.category} corruption in native-issued execution')
