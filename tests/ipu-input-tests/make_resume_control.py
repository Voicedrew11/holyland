# SPDX-License-Identifier: GPL-3.0-or-later
"""Restore only the old IPU CHCR resume cache in a build-tree source copy."""
import pathlib
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
fixed = """        if (address == 0x1000B400u && (value & 0x100u) != 0u)
        {
            // A resumed chain uses the current CHCR tag, which may have
            // changed while suspended; QWC zero instead fetches TADR.
            m_ipuInputTagLoaded = false;
            m_ipuInputEndAfterPayload = false;
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
"""
prior = """        if (address == 0x1000B400u && (value & 0x100u) != 0u)
        {
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
"""
if source.count(fixed) != 1:
    raise SystemExit("Expected exactly one fixed IPU CHCR start block")
destination = pathlib.Path(sys.argv[2])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text(source.replace(fixed, prior, 1), encoding="utf-8")
