# SPDX-License-Identifier: GPL-3.0-or-later
"""Restore only active IPU CHCR overwrites in a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
fixed = """    if (address == 0x1000B400u)
    {
        const auto current = m_ioRegisters.find(address);
        if (current != m_ioRegisters.end() && (current->second & 0x100u) != 0u)
        {
            // While active, only STR may be cleared. Preserve the fetched
            // tag and channel fields when the guest suspends input DMA.
            if ((value & 0x100u) == 0u)
                current->second &= ~0x100u;
            return true;
        }
    }

"""
if source.count(fixed) != 1:
    raise SystemExit("Expected exactly one active IPU CHCR write guard")
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(source.replace(fixed, "", 1), encoding="utf-8")
