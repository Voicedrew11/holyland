#!/usr/bin/env python3
"""Require the intended differential failure, rather than accepting any failure."""
import argparse
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--executable", required=True)
parser.add_argument("--category", choices=("registers", "flags", "cycles", "memory", "packet", "decoded-metadata"), required=True)
arguments = parser.parse_args()
mode = "--metadata-control" if arguments.category == "decoded-metadata" else "--control=" + arguments.category
result = subprocess.run([arguments.executable, mode],
    capture_output=True, text=True, timeout=120)
sys.stdout.write(result.stdout)
sys.stderr.write(result.stderr)
expected = re.compile(r"^MISMATCH call=\d+ category=" + arguments.category + r"$", re.MULTILINE)
if result.returncode != 1 or not expected.search(result.stderr):
    parser.exit(1, f"Expected exit 1 and the {arguments.category} differential mismatch; "
        f"got exit {result.returncode}.\n")
print(f"PASS detected {arguments.category} corruption")
