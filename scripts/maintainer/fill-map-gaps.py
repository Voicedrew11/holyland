#!/usr/bin/env python3
"""fill-map-gaps.py — MAINTAINERS ONLY. Add functions Ghidra missed to the map.

Usage: fill-map-gaps.py <elf> <functions.csv> [--write]

Ghidra's auto-analysis misses code that is only reached through function
pointer tables (e.g. command/handler tables), so calls to it end in
"[guest-branch:missing-target] ... op=JALR". This scans .text for spans not
covered by the CSV that contain non-nop words and splits them into functions:
a function starts at the first non-nop word, or at any address that a data
word (.data/.rodata/.sdata) or a jal points to; it ends after a jr $ra or
j (plus delay slot) once no branch inside it targets a later address.
Second pass: a data word or jal target that lands INSIDE an existing
function, right after a "jr $ra; <delay slot>; [nop padding]" boundary, is a
function Ghidra merged into its predecessor; it gets its own entry too.
Prints the rows it would add; --write appends them to the CSV.
"""
import csv, struct, sys

def main():
    elf, csv_path = sys.argv[1], sys.argv[2]
    write = '--write' in sys.argv
    data = open(elf, 'rb').read()
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', data, shoff + i * shentsize)
        secs.append((name, typ, flags, addr, off, size))
    strtab_off = secs[shstrndx][4]
    def secname(n):
        end = data.index(b'\0', strtab_off + n)
        return data[strtab_off + n:end].decode()
    sec = {secname(s[0]): s for s in secs}
    _, _, _, text_addr, text_off, text_size = sec['.text']
    text_end = text_addr + text_size

    def word(a):
        return struct.unpack_from('<I', data, text_off + a - text_addr)[0]

    rows = list(csv.reader(open(csv_path)))
    header, body = rows[0], rows[1:]
    known = {int(r[1], 16) for r in body}
    spans = sorted((int(r[1], 16), int(r[2], 16)) for r in body)
    merged = []
    for s, e in spans:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    gaps = []
    prev_end = text_addr
    for s, e in merged:
        if s > prev_end:
            gaps.append((prev_end, s))
        prev_end = max(prev_end, e)
    if prev_end < text_end:
        gaps.append((prev_end, text_end))

    def in_gap(a):
        return any(g0 <= a < g1 for g0, g1 in gaps)

    # Forced starts: data words and jal targets that land in a gap.
    forced = set()
    for name in ('.data', '.rodata', '.sdata'):
        if name not in sec:
            continue
        _, _, _, addr, off, size = sec[name]
        for i in range(0, size - 3, 4):
            v, = struct.unpack_from('<I', data, off + i)
            if v % 4 == 0 and text_addr <= v < text_end and in_gap(v) and word(v) != 0:
                forced.add(v)
    for a in range(text_addr, text_end, 4):
        w = word(a)
        if (w >> 26) == 3:  # jal
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if in_gap(t) and word(t) != 0:
                forced.add(t)

    def branch_target(a, w):
        op = w >> 26
        rs = (w >> 21) & 31
        rt = (w >> 16) & 31
        imm = w & 0xFFFF
        if imm & 0x8000:
            imm -= 0x10000
        rel = a + 4 + (imm << 2)
        if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
            return rel
        if op == 1 and rt in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13):
            return rel
        if op in (0x11, 0x12) and rs == 8:  # bc1x / bc2x
            return rel
        if op == 2:  # j
            return ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
        return None

    new = []
    for g0, g1 in gaps:
        a = g0
        while a < g1:
            if word(a) == 0 and a not in forced:
                a += 4
                continue
            start = a
            furthest = start
            end = None
            a = start
            while a < g1:
                if a != start and a in forced:
                    end = a
                    break
                w = word(a)
                t = branch_target(a, w)
                if t is not None and start <= t < g1 and (w >> 26) != 2:
                    furthest = max(furthest, t)
                is_jr_ra = w == 0x03E00008
                is_j_out = (w >> 26) == 2 and t is not None and not (start <= t < g1)
                if (is_jr_ra or is_j_out) and furthest <= a + 4:
                    end = min(a + 8, g1)
                    break
                a += 4
            if end is None:
                end = a
            new.append((start, end))
            a = end
    new = [(s, e) for s, e in new if s not in known and e > s]

    # Second pass: pointer/jal targets inside existing functions that sit on a
    # function boundary (previous non-nop instruction pair is jr $ra + delay).
    def all_targets():
        for name in ('.data', '.rodata', '.sdata'):
            if name not in sec:
                continue
            _, _, _, addr, off, size = sec[name]
            for i in range(0, size - 3, 4):
                v, = struct.unpack_from('<I', data, off + i)
                if v % 4 == 0 and text_addr <= v < text_end:
                    yield v
        for a in range(text_addr, text_end, 4):
            w = word(a)
            if (w >> 26) == 3:
                yield ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)

    def boundary_before(t):
        a = t - 4
        while a >= text_addr and word(a) == 0:
            a -= 4
        # a is the last non-nop word: either the delay slot of jr $ra, or
        # (if the delay slot was a nop) the jr $ra itself.
        return (a - 4 >= text_addr and word(a - 4) == 0x03E00008) or word(a) == 0x03E00008

    new_starts = {s for s, _ in new}
    inner = []
    for t in sorted(set(all_targets())):
        if t in known or t in new_starts or word(t) == 0 or in_gap(t):
            continue
        if not boundary_before(t):
            continue
        # end at the end of the enclosing function
        enclosing = [e for s, e in spans if s < t < e]
        if not enclosing:
            continue
        inner.append((t, max(enclosing)))
    new += inner
    inner_starts = {s for s, _ in inner}
    out = ['%s_%08x,0x%08X,0x%08X,%d' % ('entry' if s in inner_starts else 'FUN', s, s, e, e - s) for s, e in new]
    print('\n'.join(out))
    print('# %d functions (%d inside merged ranges) in %d gaps (%d forced starts)' % (len(out), len(inner), len(gaps), len(forced)), file=sys.stderr)
    if write and out:
        with open(csv_path, 'a') as f:
            f.write('\n'.join(out) + '\n')

if __name__ == '__main__':
    main()
