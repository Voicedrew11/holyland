#!/usr/bin/env python3
"""VU1 microcode disassembler and control-flow-graph builder.

Usage: vu1dis.py <image.bin> [--entry 0x0000 ...] [--cfg]

Input is a raw VU1 micro memory dump (16 KB, 8-byte pairs: lower word at +0,
upper word at +4). Mnemonics follow the VU manual. Output is for analysis only
(never committed: it embeds game microcode).
"""
import struct
import sys

DEST = "xyzw"


def dest(w):
    d = (w >> 21) & 0xF
    return "".join(c for c, b in zip(DEST, (8, 4, 2, 1)) if d & b) or "."


def vf(n):
    return f"vf{n:02d}"


def vi(n):
    return f"vi{n:02d}"


def fsf(w):
    return DEST[(w >> 21) & 3]


def ftf(w):
    return DEST[(w >> 23) & 3]


BC = "xyzw"
UP_BASIC = {
    0x00: "ADDx", 0x01: "ADDy", 0x02: "ADDz", 0x03: "ADDw",
    0x04: "SUBx", 0x05: "SUBy", 0x06: "SUBz", 0x07: "SUBw",
    0x08: "MADDx", 0x09: "MADDy", 0x0A: "MADDz", 0x0B: "MADDw",
    0x0C: "MSUBx", 0x0D: "MSUBy", 0x0E: "MSUBz", 0x0F: "MSUBw",
    0x10: "MAXx", 0x11: "MAXy", 0x12: "MAXz", 0x13: "MAXw",
    0x14: "MINIx", 0x15: "MINIy", 0x16: "MINIz", 0x17: "MINIw",
    0x18: "MULx", 0x19: "MULy", 0x1A: "MULz", 0x1B: "MULw",
    0x1C: "MULq", 0x1D: "MAXi", 0x1E: "MULi", 0x1F: "MINIi",
    0x20: "ADDq", 0x21: "MADDq", 0x22: "ADDi", 0x23: "MADDi",
    0x24: "SUBq", 0x25: "MSUBq", 0x26: "SUBi", 0x27: "MSUBi",
    0x28: "ADD", 0x29: "MADD", 0x2A: "MUL", 0x2B: "MAX",
    0x2C: "SUB", 0x2D: "MSUB", 0x2E: "OPMSUB", 0x2F: "MINI",
}
# 4-wide families at op 0x3C..0x3F; key = (op&3) | (op bits 6..10 -> <<2)
UP_SPECIAL = {
    0x00: "ADDAx", 0x01: "ADDAy", 0x02: "ADDAz", 0x03: "ADDAw",
    0x04: "SUBAx", 0x05: "SUBAy", 0x06: "SUBAz", 0x07: "SUBAw",
    0x08: "MADDAx", 0x09: "MADDAy", 0x0A: "MADDAz", 0x0B: "MADDAw",
    0x0C: "MSUBAx", 0x0D: "MSUBAy", 0x0E: "MSUBAz", 0x0F: "MSUBAw",
    0x10: "ITOF0", 0x11: "ITOF4", 0x12: "ITOF12", 0x13: "ITOF15",
    0x14: "FTOI0", 0x15: "FTOI4", 0x16: "FTOI12", 0x17: "FTOI15",
    0x18: "MULAx", 0x19: "MULAy", 0x1A: "MULAz", 0x1B: "MULAw",
    0x1C: "MULAq", 0x1D: "ABS", 0x1E: "MULAi", 0x1F: "CLIP",
    0x20: "ADDAq", 0x21: "MADDAq", 0x22: "ADDAi", 0x23: "MADDAi",
    0x24: "SUBAq", 0x25: "MSUBAq", 0x26: "SUBAi", 0x27: "MSUBAi",
    0x28: "ADDA", 0x29: "MADDA", 0x2A: "MULA", 0x2C: "SUBA",
    0x2D: "MSUBA", 0x2E: "OPMULA", 0x2F: "NOP",
}


def upper_dis(w):
    op = w & 0x3F
    d, fs, ft, fd = dest(w), (w >> 11) & 31, (w >> 16) & 31, (w >> 6) & 31
    if op <= 0x2F:
        name = UP_BASIC.get(op)
        if name is None:
            return f"?UP{op:02x}"
        if name.endswith(("q", "i")) and name not in ("MAXi", "MINIi", "MULi"):
            pass
        base = name
        if name[-1] in "xyzw" and op <= 0x1B:
            return f"{name}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}"
        if name in ("MULq", "MAXi", "MULi", "MINIi") or name[-1] in "qi":
            src = "Q" if name[-1] == "q" else "I"
            return f"{base}.{d} {vf(fd)}, {vf(fs)}, {src}"
        if name == "OPMSUB":
            return f"OPMSUB.xyz {vf(fd)}, {vf(fs)}, {vf(ft)}"
        return f"{name}.{d} {vf(fd)}, {vf(fs)}, {vf(ft)}"
    sp = (w & 3) | ((w >> 4) & 0x7C)
    name = UP_SPECIAL.get(sp)
    if name is None:
        return f"?UPSP{sp:02x}"
    ac = vf(fs)
    if name == "NOP":
        return "NOP"
    if name == "CLIP":
        return f"CLIP.xyz {vf(fs)}, {vf(ft)}w"
    if name.startswith(("ITOF", "FTOI", "ABS")):
        return f"{name}.{d} {vf(ft)}, {vf(fs)}"
    if name == "OPMULA":
        return f"OPMULA.xyz ACC, {vf(fs)}, {vf(ft)}"
    if name[-1] in "xyzw" and sp <= 0x1B:
        return f"{name}.{d} ACC, {vf(fs)}, {vf(ft)}"
    if name[-1] in "qi":
        return f"{name}.{d} ACC, {vf(fs)}, {'Q' if name[-1]=='q' else 'I'}"
    return f"{name}.{d} ACC, {vf(fs)}, {vf(ft)}"


def s11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def imm15(w):
    return ((w >> 10) & 0x7800) | (w & 0x7FF)


def lower_dis(w, pc):
    """Returns (text, kind, target). kind: None|'b'(cond)|'br'|'jr'|'jalr'|'bal'|'xgkick'."""
    if w == 0x8000033C or w == 0:
        return "NOP", None, None
    hi = (w >> 25) & 0x7F
    d = dest(w)
    ft, fs = (w >> 16) & 31, (w >> 11) & 31
    it, is_, id_ = (w >> 16) & 15, (w >> 11) & 15, (w >> 6) & 15
    fsfv = fsf(w)
    ftfv = ftf(w)
    if hi == 0x00:
        return f"LQ.{d} {vf(ft)}, {s11(w)}({vi(is_)})", None, None
    if hi == 0x01:
        return f"SQ.{d} {vf(fs)}, {s11(w)}({vi(it)})", None, None
    if hi == 0x04:
        return f"ILW.{d} {vi(it)}, {s11(w)}({vi(is_)})", None, None
    if hi == 0x05:
        return f"ISW.{d} {vi(it)}, {s11(w)}({vi(is_)})", None, None
    if hi == 0x08:
        return f"IADDIU {vi(it)}, {vi(is_)}, {imm15(w)}", None, None
    if hi == 0x09:
        return f"ISUBIU {vi(it)}, {vi(is_)}, {imm15(w)}", None, None
    if hi == 0x10:
        return f"FCEQ vi01, 0x{w & 0xFFFFFF:06x}", None, None
    if hi == 0x11:
        return f"FCSET 0x{w & 0xFFFFFF:06x}", None, None
    if hi == 0x12:
        return f"FCAND vi01, 0x{w & 0xFFFFFF:06x}", None, None
    if hi == 0x13:
        return f"FCOR vi01, 0x{w & 0xFFFFFF:06x}", None, None
    if hi == 0x14:
        return f"FSEQ {vi(it)}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", None, None
    if hi == 0x15:
        return f"FSSET 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", None, None
    if hi == 0x16:
        return f"FSAND {vi(it)}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", None, None
    if hi == 0x17:
        return f"FSOR {vi(it)}, 0x{((w >> 10) & 0x800) | (w & 0x7FF):03x}", None, None
    if hi == 0x18:
        return f"FMEQ {vi(it)}, {vi(is_)}", None, None
    if hi == 0x1A:
        return f"FMAND {vi(it)}, {vi(is_)}", None, None
    if hi == 0x1B:
        return f"FMOR {vi(it)}, {vi(is_)}", None, None
    if hi == 0x1C:
        return f"FCGET {vi(it)}", None, None
    tgt = (pc + 8 + s11(w) * 8) & 0x3FFF
    if hi == 0x20:
        return f"B 0x{tgt:04x}", "br", tgt
    if hi == 0x21:
        return f"BAL {vi(it)}, 0x{tgt:04x}", "bal", tgt
    if hi == 0x24:
        return f"JR {vi(is_)}", "jr", None
    if hi == 0x25:
        return f"JALR {vi(it)}, {vi(is_)}", "jalr", None
    names = {0x28: "IBEQ", 0x29: "IBNE", 0x2C: "IBLTZ", 0x2D: "IBGTZ", 0x2E: "IBLEZ", 0x2F: "IBGEZ"}
    if hi in (0x28, 0x29):
        return f"{names[hi]} {vi(it)}, {vi(is_)}, 0x{tgt:04x}", "b", tgt
    if hi in names:
        return f"{names[hi]} {vi(is_)}, 0x{tgt:04x}", "b", tgt
    if hi == 0x40:
        direct = w & 0x3F
        if direct == 0x30:
            return f"IADD {vi(id_)}, {vi(is_)}, {vi(it)}", None, None
        if direct == 0x31:
            return f"ISUB {vi(id_)}, {vi(is_)}, {vi(it)}", None, None
        if direct == 0x32:
            return f"IADDI {vi(it)}, {vi(is_)}, {((w >> 6) & 31) - (32 if (w >> 6) & 16 else 0)}", None, None
        if direct == 0x34:
            return f"IAND {vi(id_)}, {vi(is_)}, {vi(it)}", None, None
        if direct == 0x35:
            return f"IOR {vi(id_)}, {vi(is_)}, {vi(it)}", None, None
        if direct < 0x3C:
            return f"?LOW{direct:02x}", None, None
        sp = (w & 3) | ((w >> 4) & 0x7C)
        t = {
            0x30: f"MOVE.{d} {vf(ft)}, {vf(fs)}",
            0x31: f"MR32.{d} {vf(ft)}, {vf(fs)}",
            0x34: f"LQI.{d} {vf(ft)}, ({vi(is_)}++)",
            0x35: f"SQI.{d} {vf(fs)}, ({vi(it)}++)",
            0x36: f"LQD.{d} {vf(ft)}, (--{vi(is_)})",
            0x37: f"SQD.{d} {vf(fs)}, (--{vi(it)})",
            0x38: f"DIV Q, {vf(fs)}{fsfv}, {vf(ft)}{ftfv}",
            0x39: f"SQRT Q, {vf(ft)}{ftfv}",
            0x3A: f"RSQRT Q, {vf(fs)}{fsfv}, {vf(ft)}{ftfv}",
            0x3B: "WAITQ",
            0x3C: f"MTIR {vi(it)}, {vf(fs)}{fsfv}",
            0x3D: f"MFIR.{d} {vf(ft)}, {vi(is_)}",
            0x3E: f"ILWR.{d} {vi(it)}, ({vi(is_)})",
            0x3F: f"ISWR.{d} {vi(it)}, ({vi(is_)})",
            0x40: f"RNEXT.{d} {vf(ft)}, R",
            0x41: f"RGET.{d} {vf(ft)}, R",
            0x42: f"RINIT R, {vf(fs)}{fsfv}",
            0x43: f"RXOR R, {vf(fs)}{fsfv}",
            0x64: f"MFP.{d} {vf(ft)}, P",
            0x68: f"XTOP {vi(it)}",
            0x69: f"XITOP {vi(it)}",
            0x6C: f"XGKICK {vi(is_)}",
            0x70: f"ESADD P, {vf(fs)}",
            0x71: f"ERSADD P, {vf(fs)}",
            0x72: f"ELENG P, {vf(fs)}",
            0x73: f"ERLENG P, {vf(fs)}",
            0x74: f"EATANxy P, {vf(fs)}",
            0x75: f"EATANxz P, {vf(fs)}",
            0x76: f"ESUM P, {vf(fs)}",
            0x78: f"ESQRT P, {vf(fs)}{fsfv}",
            0x79: f"ERSQRT P, {vf(fs)}{fsfv}",
            0x7A: f"ERCPR P, {vf(fs)}{fsfv}",
            0x7B: "WAITP",
            0x7C: f"ESIN P, {vf(fs)}{fsfv}",
            0x7D: f"EATAN P, {vf(fs)}{fsfv}",
            0x7E: f"EEXP P, {vf(fs)}{fsfv}",
        }.get(sp)
        if t is None:
            return f"?LOWSP{sp:02x}", None, None
        return t, ("xgkick" if sp == 0x6C else None), None
    return f"?LOWHI{hi:02x}", None, None


class Pair:
    def __init__(self, img, pc):
        self.pc = pc
        self.lower, self.upper = struct.unpack_from("<II", img, pc)
        self.i = bool(self.upper & 0x80000000)
        self.e = bool(self.upper & 0x40000000)
        self.m = bool(self.upper & 0x20000000)
        self.d = bool(self.upper & 0x10000000)
        self.t = bool(self.upper & 0x08000000)
        self.utext = upper_dis(self.upper)
        if self.i:
            self.ltext, self.kind, self.target = (
                f"LOI {struct.unpack('<f', struct.pack('<I', self.lower))[0]!r}", None, None)
        else:
            self.ltext, self.kind, self.target = lower_dis(self.lower, pc)

    def flags(self):
        s = ""
        for c, b in zip("IEMDT", (self.i, self.e, self.m, self.d, self.t)):
            if b:
                s += c
        return s

    def line(self):
        f = self.flags()
        return f"{self.pc:04x}: {self.utext:<44}| {self.ltext}" + (f"   [{f}]" if f else "")


def build_cfg(img, entry):
    """Block leaders are entry, branch targets, and the pc after a branch's delay slot.

    E-bit ends flow after one more pair (the delay slot after E). A branch
    executes its delay slot, then jumps. Returns {leader: (end_pc, succs, note)}.
    """
    n = len(img) // 8
    pairs = {}

    def pair(pc):
        if pc not in pairs:
            pairs[pc] = Pair(img, pc)
        return pairs[pc]

    leaders = {entry}
    work = [entry]
    blocks = {}
    resumes = set()
    while work:
        start = work.pop()
        if start in blocks:
            continue
        pc = start
        e_seen = False
        while True:
            if pc >= len(img):
                blocks[start] = (pc - 8, [], "falls off end")
                break
            p = pair(pc)
            if e_seen:
                # MSCNT resumes at the pair after the E delay slot.
                blocks[start] = (pc, [], f"E-end (resume 0x{pc + 8:04x})")
                resumes.add(pc + 8)
                break
            if p.kind in ("b", "br", "bal"):
                slot = pc + 8
                succs = []
                note = p.kind
                if p.kind != "br":
                    succs.append(slot + 8)
                succs.append(p.target)
                blocks[start] = (slot, succs, note + (" +E" if p.e else ""))
                for s in succs:
                    if s not in blocks:
                        leaders.add(s)
                        work.append(s)
                break
            if p.kind in ("jr", "jalr"):
                blocks[start] = (pc + 8, [], p.kind + " (indirect)")
                break
            if p.e:
                e_seen = True
            pc += 8
    return pairs, blocks, resumes


def split_blocks(blocks, leaders_extra=()):
    """Split overlapping blocks at every other block's start (true basic blocks)."""
    starts = sorted(set(blocks) | set(leaders_extra))
    out = {}
    for s, (end, succs, note) in blocks.items():
        cuts = [x for x in starts if s < x < end]  # end is the delay slot: stays in this block (shared pair is duplicated)
        cur = s
        for c in cuts:
            out[cur] = (c - 8, [c], "fall")
            cur = c
        out[cur] = (end, succs, note)
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    img = open(args[0], "rb").read()
    entries, cfg = [], False
    i = 1
    while i < len(args):
        if args[i] == "--entry":
            entries.append(int(args[i + 1], 0))
            i += 2
        elif args[i] == "--cfg":
            cfg = True
            i += 1
        else:
            i += 1
    if not entries:
        for pc in range(0, len(img), 8):
            print(Pair(img, pc).line())
        return 0
    for e in entries:
        # Close over MSCNT resume points: each is another segment entry.
        blocks, resumes, todo = {}, set(), [e]
        while todo:
            seg = todo.pop()
            _, b, r = build_cfg(img, seg)
            for k, v in b.items():
                blocks.setdefault(k, v)
            for x in sorted(r - resumes):
                resumes.add(x)
                if x < len(img):
                    todo.append(x)
        blocks = split_blocks(blocks)
        print(f"; entry 0x{e:04x}: {len(blocks)} blocks; resume points {[hex(r) for r in sorted(resumes)]}")
        for s in sorted(blocks):
            end, succs, note = blocks[s]
            print(f"\n; block {s:04x}..{end:04x} -> {[hex(x) for x in succs]} {note}")
            if not cfg:
                for pc in range(s, end + 8, 8):
                    print(Pair(img, pc).line())
    return 0


if __name__ == "__main__":
    sys.exit(main())
