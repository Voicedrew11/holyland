#!/usr/bin/env python3
"""Lift a VU1 microprogram segment to C++ with a static, cycle-exact schedule.

Usage: vu1lift.py <image.bin> --segment 0x08f8 [--segment ...] -o <out.cpp>

<image.bin> is a 16 KB VU1 micro memory dump (PS2X_VU1_CENSUS writes them).
A segment is a pc where a microprogram run starts with quiescent pipelines:
an MSCAL entry, or an MSCNT resume point (the pair after an E bit's delay
slot). The output embeds the game's program logic, so it is never committed;
it is compiled into the runner from <game-dir>/vu1lift (dev-build.sh).

The timing model is a port of the PS2Recomp VU1 interpreter
(ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp): decodeUpperUsage /
decodeLowerUsage, the per-pair ready-slot stalls (calculatePairReadyCycle,
markPairWrites) and the flag / FDIV / EFU pipelines. Every block is
simulated once per distinct pipeline state at its entry, so each emitted
block instance has constant issue cycles. What the lifted code computes must
match the interpreter bit for bit, including cycle counts; check it with the
VU1 verifier (docs/NOTES.md, "Dev loop").

Not supported yet (the generator stops with an error): reads of MAC/status
flags (FMAND, FMEQ, FMOR, FSAND, FSEQ, FSOR), FSSET, OPMULA/OPMSUB, JR/JALR,
BAL, D/T bits, RNEXT/RGET/RINIT/RXOR, and an XGKICK that may stall when
later pairs still depend on timing.

Ported from PS2Recomp sources (GPL-3.0), so, like patches/, it is
distributed under the same license.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1dis  # noqa: E402

FMAC_LATENCY = 4
ACC_FORWARD_LATENCY = 1
READY_VI = 128
READY_ACC = 144


class LiftError(Exception):
    pass


def lane(c):
    """VU dest bit for component c (x=8, y=4, z=2, w=1)."""
    return 1 << (3 - c)


def comps(mask):
    return [c for c in range(4) if mask & lane(c)]


def blend_imm(mask):
    """_mm_blend_ps immediate for a VU dest mask (bit i = lane i)."""
    return sum(1 << c for c in comps(mask))


def fnv1a(data):
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def s11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def f32bits(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def normalize_bits(bits):
    exp = (bits >> 23) & 0xFF
    if exp == 0:
        return bits & 0x80000000
    if exp == 0xFF:
        return (bits & 0x80000000) | 0x7F7FFFFF
    return bits


# --------------------------------------------------------------------------
# Usage decode: a direct port of decodeUpperUsage / decodeLowerUsage.
# --------------------------------------------------------------------------
class Usage:
    def __init__(self):
        self.vf_read = []  # [(reg, lanes)]
        self.vf_write = (0, 0)
        self.vi_read = 0
        self.vi_write = 0
        self.acc_read = 0
        self.acc_write = 0
        self.latency = 0
        self.vf_latency = 0
        self.vi_latency = 0
        self.pipeline = None
        self.wait_q = False
        self.wait_p = False
        self.reads_clip = False
        self.writes_clip = False
        self.delays_branch = False
        self.reserved = False

    def add_vf_read(self, reg, lanes):
        if lanes == 0:
            return
        for i, (r, l) in enumerate(self.vf_read):
            if r == reg:
                self.vf_read[i] = (r, l | lanes)
                return
        if len(self.vf_read) < 2:
            self.vf_read.append((reg, lanes))

    def add_vf_write(self, reg, lanes):
        if reg == 0 or lanes == 0:
            return
        if self.vf_write[0] == 0:
            self.vf_write = (reg, lanes)
        elif self.vf_write[0] == reg:
            self.vf_write = (reg, self.vf_write[1] | lanes)

    def vf_read_lanes(self, reg):
        for r, l in self.vf_read:
            if r == reg:
                return l
        return 0


def decode_upper_usage(upper):
    u = Usage()
    u.pipeline = "fmac"
    u.latency = FMAC_LATENCY
    op = upper & 0x3F
    dest = (upper >> 21) & 0xF
    fs, ft, fd = (upper >> 11) & 31, (upper >> 16) & 31, (upper >> 6) & 31
    if op <= 0x2F:
        u.add_vf_read(fs, dest)
        u.add_vf_write(fd, dest)
        if op <= 0x1B:
            u.add_vf_read(ft, lane(op & 3))
        elif op >= 0x28:
            u.add_vf_read(ft, 0xE if op == 0x2E else dest)
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                  0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E):
            u.acc_read = dest
        return u
    if op >= 0x3C:
        sp = (upper & 3) | ((upper >> 4) & 0x7C)
        writes_acc = (sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or
                      0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E)
        if writes_acc:
            u.add_vf_read(fs, dest)
            if sp <= 0x1B:
                u.add_vf_read(ft, lane(sp & 3))
            elif 0x28 <= sp <= 0x2E:
                u.add_vf_read(ft, 0xE if sp == 0x2E else dest)
            u.acc_write = dest
            if (0x08 <= sp <= 0x0F) or sp in (0x21, 0x23, 0x25, 0x27, 0x29, 0x2D):
                u.acc_read = dest
        elif 0x10 <= sp <= 0x17 or sp == 0x1D:
            u.add_vf_read(fs, dest)
            u.add_vf_write(ft, dest)
        elif sp == 0x1F:
            u.add_vf_read(fs, 0xE)
            u.add_vf_read(ft, 0x1)
            u.writes_clip = True
        elif sp not in (0x2F, 0x30):
            u.reserved = True
        return u
    u.reserved = True
    return u


EFU_LATENCY = {0x70: 11, 0x71: 18, 0x72: 18, 0x79: 18, 0x73: 24, 0x74: 54, 0x75: 54,
               0x7D: 54, 0x76: 12, 0x78: 12, 0x7A: 12, 0x7C: 29, 0x7E: 44}


def decode_lower_usage(lower):
    u = Usage()
    if lower == 0 or lower == 0x8000033C:
        return u
    hi = (lower >> 25) & 0x7F
    vft, vfs = (lower >> 16) & 31, (lower >> 11) & 31
    vit, vis, vid = (lower >> 16) & 15, (lower >> 11) & 15, (lower >> 6) & 15
    dest = (lower >> 21) & 0xF

    def rvi(r):
        if r:
            u.vi_read |= 1 << r

    def wvi(r):
        if r:
            u.vi_write |= 1 << r

    if hi == 0x00:
        u.pipeline, u.latency = "lsu", 4
        rvi(vis)
        u.add_vf_write(vft, dest)
    elif hi == 0x01:
        u.pipeline, u.latency = "lsu", 1
        rvi(vit)
        u.add_vf_read(vfs, dest)
    elif hi == 0x04:
        u.pipeline, u.latency = "lsu", 4
        rvi(vis)
        wvi(vit)
    elif hi == 0x05:
        u.pipeline, u.latency = "lsu", 1
        rvi(vis)
        rvi(vit)
    elif hi in (0x08, 0x09):
        u.pipeline, u.latency, u.delays_branch = "ialu", 1, True
        rvi(vis)
        wvi(vit)
    elif hi in (0x10, 0x12, 0x13):
        u.pipeline, u.latency, u.reads_clip = "ialu", 1, True
        wvi(1)
    elif hi == 0x11:
        u.pipeline, u.latency, u.writes_clip = "fmac", FMAC_LATENCY, True
    elif hi in (0x14, 0x16, 0x17):
        u.pipeline, u.latency = "ialu", 1
        wvi(vit)
    elif hi == 0x15:
        u.pipeline, u.latency = "fmac", FMAC_LATENCY
    elif hi in (0x18, 0x1A, 0x1B):
        u.pipeline, u.latency = "ialu", 1
        rvi(vis)
        wvi(vit)
    elif hi == 0x1C:
        u.pipeline, u.latency, u.reads_clip = "ialu", 1, True
        wvi(vit)
    elif hi == 0x20:
        u.pipeline = "branch"
    elif hi == 0x21:
        u.pipeline, u.latency = "branch", 1
        wvi(vit)
    elif hi == 0x24:
        u.pipeline = "branch"
        rvi(vis)
    elif hi == 0x25:
        u.pipeline, u.latency = "branch", 1
        rvi(vis)
        wvi(vit)
    elif hi in (0x28, 0x29):
        u.pipeline = "branch"
        rvi(vis)
        rvi(vit)
    elif hi in (0x2C, 0x2D, 0x2E, 0x2F):
        u.pipeline = "branch"
        rvi(vis)
    elif hi == 0x40:
        direct = lower & 0x3F
        if direct in (0x30, 0x31, 0x34, 0x35):
            u.pipeline, u.latency, u.delays_branch = "ialu", 1, True
            rvi(vis)
            rvi(vit)
            wvi(vid)
        elif direct == 0x32:
            u.pipeline, u.latency, u.delays_branch = "ialu", 1, True
            rvi(vis)
            wvi(vit)
        elif direct < 0x3C:
            u.reserved = True
        else:
            sp = (lower & 3) | ((lower >> 4) & 0x7C)
            fsf = lane((lower >> 21) & 3)
            ftf = lane((lower >> 23) & 3)
            if sp in (0x30, 0x31):
                u.pipeline, u.latency = "fmac", 4
                u.add_vf_read(vfs, 0xF if sp == 0x31 else dest)
                u.add_vf_write(vft, dest)
            elif sp in (0x34, 0x36):
                u.pipeline, u.latency, u.vi_latency, u.delays_branch = "lsu", 4, 1, True
                rvi(vis)
                wvi(vis)
                u.add_vf_write(vft, dest)
            elif sp in (0x35, 0x37):
                u.pipeline, u.latency, u.delays_branch = "lsu", 1, True
                rvi(vit)
                wvi(vit)
                u.add_vf_read(vfs, dest)
            elif sp == 0x38:
                u.pipeline, u.latency = "fdiv", 7
                u.add_vf_read(vfs, fsf)
                u.add_vf_read(vft, ftf)
            elif sp == 0x39:
                u.pipeline, u.latency = "fdiv", 7
                u.add_vf_read(vft, ftf)
            elif sp == 0x3A:
                u.pipeline, u.latency = "fdiv", 13
                u.add_vf_read(vfs, fsf)
                u.add_vf_read(vft, ftf)
            elif sp == 0x3B:
                u.pipeline, u.wait_q = "fdiv", True
            elif sp == 0x3C:
                u.pipeline, u.latency, u.delays_branch = "ialu", 1, True
                u.add_vf_read(vfs, fsf)
                wvi(vit)
            elif sp == 0x3D:
                u.pipeline, u.latency = "fmac", 4
                rvi(vis)
                u.add_vf_write(vft, dest)
            elif sp == 0x3E:
                u.pipeline, u.latency = "lsu", 4
                rvi(vis)
                wvi(vit)
            elif sp == 0x3F:
                u.pipeline, u.latency = "lsu", 1
                rvi(vis)
                rvi(vit)
            elif sp in (0x40, 0x41):
                u.pipeline, u.latency = "fmac", 4
                u.add_vf_write(vft, dest)
            elif sp in (0x42, 0x43):
                u.pipeline, u.latency = "ialu", 1
                u.add_vf_read(vfs, fsf)
            elif sp == 0x64:
                u.pipeline, u.latency = "fmac", 4
                u.add_vf_write(vft, dest)
            elif sp in (0x68, 0x69):
                u.pipeline, u.latency = "ialu", 1
                wvi(vit)
            elif sp == 0x6C:
                u.pipeline, u.latency = "xgkick", 2
                rvi(vis)
            elif sp in EFU_LATENCY:
                u.pipeline, u.latency = "efu", EFU_LATENCY[sp]
                if 0x70 <= sp <= 0x73:
                    u.add_vf_read(vfs, 0xE)
                elif sp == 0x74:
                    u.add_vf_read(vfs, 0xC)
                elif sp == 0x75:
                    u.add_vf_read(vfs, 0xA)
                elif sp == 0x76:
                    u.add_vf_read(vfs, 0xF)
                else:
                    u.add_vf_read(vfs, fsf)
            elif sp == 0x7B:
                u.pipeline, u.wait_p = "efu", True
            else:
                u.reserved = True
    else:
        u.reserved = True
    return u


class PairInfo:
    """decodeInstructionPair: usages, shadowing and the ready slots."""

    def __init__(self, img, pc):
        self.pc = pc
        self.dis = vu1dis.Pair(img, pc)
        self.lower, self.upper = struct.unpack_from("<II", img, pc)
        self.ibit = bool(self.upper & 0x80000000)
        self.ebit = bool(self.upper & 0x40000000)
        self.dbit = bool(self.upper & 0x10000000)
        self.tbit = bool(self.upper & 0x08000000)
        self.uu = decode_upper_usage(self.upper)
        self.lu = Usage() if self.ibit else decode_lower_usage(self.lower)
        uw = self.uu.vf_write[0]
        self.shadow = 0
        self.suppressed_lower_vf = 0
        if uw and (self.lu.vf_read_lanes(uw) or self.lu.vf_write[0] == uw):
            self.shadow = uw
            if self.lu.vf_write[0] == uw:
                self.suppressed_lower_vf = uw
        self.read_slots = []
        for us in (self.uu, self.lu):
            for reg, lanes in us.vf_read:
                if reg == 0:
                    continue
                for c in comps(lanes):
                    self.read_slots.append(reg * 4 + c)
            for r in range(1, 16):
                if us.vi_read & (1 << r):
                    self.read_slots.append(READY_VI + r)
            for c in comps(us.acc_read):
                self.read_slots.append(READY_ACC + c)
        self.writes = []  # (slot, latency)
        lw = self.lu.vf_write
        if lw[0] and self.suppressed_lower_vf != lw[0]:
            lat = self.lu.vf_latency or self.lu.latency
            for c in comps(lw[1]):
                self.writes.append((lw[0] * 4 + c, lat))
        uwr = self.uu.vf_write
        if uwr[0]:
            lat = self.uu.vf_latency or self.uu.latency
            for c in comps(uwr[1]):
                self.writes.append((uwr[0] * 4 + c, lat))
        vlat = self.lu.vi_latency or self.lu.latency
        for r in range(1, 16):
            if self.lu.vi_write & (1 << r):
                self.writes.append((READY_VI + r, vlat))
        for c in comps(self.uu.acc_write):
            self.writes.append((READY_ACC + c, ACC_FORWARD_LATENCY))
        self.max_write_latency = max((l for _, l in self.writes), default=0)
        vw = self.lu.vi_write & 0xFFFE
        self.first_vi_write = (vw & -vw).bit_length() - 1 if vw else 0
        if self.uu.reserved or self.lu.reserved:
            raise LiftError(f"0x{pc:04x}: reserved instruction")
        if self.dbit or self.tbit:
            raise LiftError(f"0x{pc:04x}: D/T bit not supported")

    # Upper op classification -------------------------------------------
    def upper_sp(self):
        op = self.upper & 0x3F
        return (self.upper & 3) | ((self.upper >> 4) & 0x7C) if op >= 0x3C else None

    def is_fmac_flag_op(self):
        """Upper ops that go through applyFmacDest[Acc] (write MAC/status)."""
        op = self.upper & 0x3F
        dest = (self.upper >> 21) & 0xF
        if dest == 0:
            return False
        if op <= 0x2F:
            return not (0x10 <= op <= 0x17 or op in (0x1D, 0x1F, 0x2B, 0x2F))
        sp = self.upper_sp()
        if sp is None:
            return False
        return (sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or
                0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2E)

    def lower_kind(self):
        if self.ibit:
            return "loi"
        w = self.lower
        if w == 0 or w == 0x8000033C:
            return "nop"
        hi = (w >> 25) & 0x7F
        if hi == 0x40:
            direct = w & 0x3F
            if direct < 0x3C:
                return {0x30: "iadd", 0x31: "isub", 0x32: "iaddi", 0x34: "iand", 0x35: "ior"}[direct]
            sp = (w & 3) | ((w >> 4) & 0x7C)
            return {0x30: "move", 0x31: "mr32", 0x34: "lqi", 0x35: "sqi", 0x36: "lqd", 0x37: "sqd",
                    0x38: "div", 0x39: "sqrt", 0x3A: "rsqrt", 0x3B: "waitq", 0x3C: "mtir", 0x3D: "mfir",
                    0x3E: "ilwr", 0x3F: "iswr", 0x64: "mfp", 0x68: "xtop", 0x69: "xitop",
                    0x6C: "xgkick", 0x7B: "waitp"}.get(sp, "efu" if sp in EFU_LATENCY else f"lsp{sp:02x}")
        return {0x00: "lq", 0x01: "sq", 0x04: "ilw", 0x05: "isw", 0x08: "iaddiu", 0x09: "isubiu",
                0x10: "fceq", 0x11: "fcset", 0x12: "fcand", 0x13: "fcor", 0x1C: "fcget",
                0x20: "b", 0x28: "ibeq", 0x29: "ibne", 0x2C: "ibltz", 0x2D: "ibgtz",
                0x2E: "iblez", 0x2F: "ibgez"}.get(hi, f"lhi{hi:02x}")

    def is_branch(self):
        return self.lower_kind() in ("b", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez")

    def branch_target(self):
        return (self.pc + 8 + s11(self.lower) * 8) & 0x3FFF


# --------------------------------------------------------------------------
# Pipeline state at a block boundary, relative to the cycle T at which the
# interpreter starts evaluating the block's first pair.
# --------------------------------------------------------------------------
class TState:
    def __init__(self):
        self.ready = {}          # slot -> cycle (only > now matters)
        self.fdiv = None         # ready cycle of the pending Q, or None
        self.efu = []            # ready cycles of pending P values, issue order
        self.efu_res = 0         # m_efuResourceReady
        self.clip = []           # ready cycles of pending clip entries, issue order
        self.pend_max = 0        # latest ready of pending flag / store entries
        self.wbh = 0             # m_writebackHorizon
        self.bk_reg = 0          # VI whose pre-write value a following branch reads
        self.kick = False        # an XGKICK may still be transferring
        self.dyn = False         # past an XGKICK stall of unknown length

    def copy(self):
        s = TState()
        s.ready = dict(self.ready)
        s.fdiv, s.efu, s.efu_res = self.fdiv, list(self.efu), self.efu_res
        s.clip, s.pend_max, s.wbh = list(self.clip), self.pend_max, self.wbh
        s.bk_reg, s.kick, s.dyn = self.bk_reg, self.kick, self.dyn
        return s

    def key(self, now):
        """Canonical form relative to now (what determines future timing)."""
        rel = lambda c: c - now if c > now else 0  # noqa: E731
        return (tuple(sorted((k, v - now) for k, v in self.ready.items() if v > now)),
                rel(self.fdiv) if self.fdiv is not None else None,
                tuple(rel(c) for c in self.efu), rel(self.efu_res),
                tuple(rel(c) for c in self.clip), rel(self.pend_max), rel(self.wbh),
                self.bk_reg, self.kick, self.dyn)

    def rebased(self, now):
        s = TState()
        s.ready = {k: v - now for k, v in self.ready.items() if v > now}
        s.fdiv = self.fdiv - now if self.fdiv is not None else None
        s.efu = [c - now for c in self.efu]
        s.efu_res = max(0, self.efu_res - now)
        s.clip = [c - now for c in self.clip]
        s.pend_max = max(0, self.pend_max - now)
        s.wbh = max(0, self.wbh - now)
        s.bk_reg, s.kick, s.dyn = self.bk_reg, self.kick, self.dyn
        return s


# --------------------------------------------------------------------------
# C++ emission helpers
# --------------------------------------------------------------------------
def vf(r):
    return "vl::kVf0" if r == 0 else f"vf{r:02d}"


def vi(r):
    return "0" if r == 0 else f"vi{r:02d}"


def fbits(bits):
    return f"vl::f32(0x{bits:08x}u)"


class Emitter:
    def __init__(self, img, segment):
        self.img = img
        self.segment = segment
        self.pairs = {}
        self.instances = {}      # (pc, key) -> name
        self.order = []          # (name, pc, state) to emit
        self.lines = []
        self.vf_used = set()
        self.vi_used = set()
        self.uses = set()

    def pair(self, pc):
        if pc not in self.pairs:
            if pc + 8 > len(self.img):
                raise LiftError(f"pc 0x{pc:04x} outside the image")
            self.pairs[pc] = PairInfo(self.img, pc)
        return self.pairs[pc]

    def instance(self, pc, state, now):
        key = (pc, state.key(now))
        if key not in self.instances:
            name = f"b{pc:04x}_{sum(1 for k in self.instances if k[0] == pc)}"
            self.instances[key] = name
            self.order.append((name, pc, state.rebased(now)))
            if len(self.instances) > 400:
                raise LiftError("block instances do not converge")
        return self.instances[key]

    # --- program walk ---------------------------------------------------
    def lift(self):
        start = TState()
        self.entry = self.instance(self.segment, start, 0)
        i = 0
        bodies = {}
        while i < len(self.order):
            name, pc, st = self.order[i]
            bodies[name] = self.block(name, pc, st)
            i += 1
        return bodies

    def block(self, name, pc, st):
        """Simulate and emit one block instance starting at pc with state st (T=0)."""
        out = []
        now = 0
        st = st.copy()
        branch = None       # (pair, taken-expression)
        in_delay = False
        e_pending = False
        while True:
            p = self.pair(pc)
            if st.dyn and (p.read_slots or p.lu.pipeline in ("fdiv", "efu", "xgkick", "lsu")
                        or p.lu.reads_clip or p.is_branch()):
                raise LiftError(f"0x{pc:04x}: depends on timing after an XGKICK that may stall")
            # Readiness (calculatePairReadyCycle). The XGKICK interlock is dynamic.
            t = now
            for s in p.read_slots:
                t = max(t, st.ready.get(s, 0))
            if p.lu.pipeline == "fdiv" and st.fdiv is not None and st.fdiv > now:
                t = max(t, st.fdiv)
            if p.lu.pipeline == "efu":
                t = max(t, st.efu_res)
            if p.lu.wait_q and st.fdiv is not None:
                t = max(t, st.fdiv)
            if p.lu.wait_p:
                for c in st.efu:
                    t = max(t, c)
            out.append(f"    // {p.dis.line()}")
            # Commits due at the issue cycle (commitDuePipelines, issue order).
            out += self.commits(st, t)
            kick_wait = p.lower_kind() == "xgkick" and st.kick
            if kick_wait:
                if st.dyn:
                    raise LiftError(f"0x{pc:04x}: second XGKICK stall")
                # Everything pending lands at a fixed cycle; record it, then
                # continue on the post-stall time base (later pairs read
                # nothing, checked above).
                out.append(f"    H = std::max(H, T + {max(st.pend_max, st.wbh, t)});")
                out.append(f"    T = vl::kickWait(c, T + {t}) - {t};")
                st.dyn = True
                st.ready, st.pend_max, st.wbh = {}, 0, 0
            out.append(f"    VL_TRACE(c, 0x{pc:04x}, T + {t});")
            out += self.semantics(p, st, t)
            # markPairWrites
            for slot, latc in p.writes:
                st.ready[slot] = t + latc
            if p.writes:
                st.wbh = max(st.wbh, t + p.max_write_latency)
            st.bk_reg = p.first_vi_write if (p.first_vi_write and p.lu.delays_branch) else 0
            now = t + 1
            if p.is_branch():
                if in_delay or e_pending:
                    raise LiftError(f"0x{pc:04x}: branch in a delay slot")
                branch = p
                in_delay = True
                pc += 8
                continue
            if in_delay:
                return out + self.branch_exit(branch, st, now)
            if e_pending:
                return out + self.end_exit(st, now, pc + 8)
            if p.ebit:
                e_pending = True
            pc += 8
            # Fall into a block another path also enters: end this block here
            # only at an existing leader, otherwise keep going.
            if not e_pending and pc in self.leaders:
                nxt = self.instance(pc, st, now)
                out.append(f"    T += {now};")
                out.append(f"    goto {nxt};")
                return out

    def commits(self, st, t):
        out = []
        if st.fdiv is not None and st.fdiv <= t:
            out.append("    q = qPend; statusDi = diPend; fl.sticky |= diPend << 6;")
            st.fdiv = None
        while st.efu and st.efu[0] <= t:
            n = len(st.efu)
            out.append("    p = pPend0;" + (" pPend0 = pPend1;" if n > 1 else ""))
            st.efu.pop(0)
        while st.clip and st.clip[0] <= t:
            n = len(st.clip)
            out.append("    clip = clipPend0;" + "".join(f" clipPend{k} = clipPend{k + 1};" for k in range(n - 1)))
            st.clip.pop(0)
        if st.pend_max <= t:
            st.pend_max = 0
        return out

    def branch_exit(self, bp, st, now):
        out = []
        kind = bp.lower_kind()
        tgt = bp.branch_target()
        nxt_fall = bp.pc + 16
        if kind == "b":
            name = self.instance(tgt, st, now)
            return [f"    T += {now};", f"    goto {name};"]
        tname = self.instance(tgt, st, now)
        fname = self.instance(nxt_fall, st, now)
        out.append(f"    T += {now};")
        out.append(f"    if (taken_{bp.pc:04x}) goto {tname};")
        out.append(f"    goto {fname};")
        return out

    def end_exit(self, st, now, end_pc):
        # flushPipelines: the run ends once every pending write has landed.
        end = max(now, st.pend_max, st.wbh)
        out = self.commits(st, 1 << 40)
        out.append(f"    H = std::max(H, T + {end});")
        out.append(f"    endPc = 0x{end_pc & 0x3FFF:04x}u;")
        out.append("    goto done;")
        return out

    # --- semantics --------------------------------------------------------
    def semantics(self, p, st, t):
        """C++ for one pair issuing at T + t. Upper result is committed after
        the lower op (the lower op reads the old register: upperVfShadowReg)."""
        out = []
        u = p.upper
        dest = (u >> 21) & 0xF
        fs, ft, fd = (u >> 11) & 31, (u >> 16) & 31, (u >> 6) & 31
        op = u & 0x3F
        commit = None   # (target-reg or 'acc', dest)
        for r in (fs, ft, fd):
            if r:
                self.vf_used.add(r)

        def nv(r):
            return "vl::kVf0" if r == 0 else f"vl::norm({vf(r)})"

        def fmac_line(kind, a, b, accexpr, d, target):
            out.append(f"    const vl::V {up} = vl::fmac<vl::{kind}, 0x{d:x}>({a}, {b}, {accexpr}, fl);")
            return (target, d)

        up = f"up{p.pc:04x}"
        sp = p.upper_sp()
        if op <= 0x2F:
            if op <= 0x1B or op >= 0x28 or op in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x1D, 0x1F):
                if op <= 0x1B:
                    b = f"vl::bc<{op & 3}>({nv(ft)})"
                elif op in (0x1C, 0x20, 0x21, 0x24, 0x25):
                    b = "vl::splat(vl::normf(q))"
                elif op in (0x1D, 0x1E, 0x1F, 0x22, 0x23, 0x26, 0x27):
                    b = "vl::splat(vl::normf(I))"
                else:
                    b = nv(ft)
                fam = None
                if op <= 0x03 or op in (0x20, 0x22, 0x28):
                    fam = "Add"
                elif op <= 0x07 or op in (0x24, 0x26, 0x2C):
                    fam = "Sub"
                elif op <= 0x0B or op in (0x21, 0x23, 0x29):
                    fam = "MAdd"
                elif op <= 0x0F or op in (0x25, 0x27, 0x2D):
                    fam = "MSub"
                elif 0x18 <= op <= 0x1B or op in (0x1C, 0x1E, 0x2A):
                    fam = "Mul"
                if fam:
                    if dest:
                        commit = fmac_line(fam, nv(fs), b, "vl::norm(acc)" if fam in ("MAdd", "MSub") else "vl::kZero", dest, fd)
                elif 0x10 <= op <= 0x13 or op in (0x1D, 0x2B):
                    out.append(f"    const vl::V {up} = vl::vmax({nv(fs)}, {b});")
                    commit = (fd, dest)
                elif 0x14 <= op <= 0x17 or op in (0x1F, 0x2F):
                    out.append(f"    const vl::V {up} = vl::vmin({nv(fs)}, {b});")
                    commit = (fd, dest)
                else:
                    raise LiftError(f"0x{p.pc:04x}: upper op {op:#x} not supported")
            else:
                raise LiftError(f"0x{p.pc:04x}: upper op {op:#x} not supported")
        else:
            if sp <= 0x0F or 0x18 <= sp <= 0x1C or sp == 0x1E or 0x20 <= sp <= 0x2A or 0x2C <= sp <= 0x2D:
                if sp <= 0x0F or 0x18 <= sp <= 0x1B:
                    b = f"vl::bc<{sp & 3}>({nv(ft)})"
                elif sp in (0x1C, 0x20, 0x21, 0x24, 0x25):
                    b = "vl::splat(vl::normf(q))"
                elif sp in (0x1E, 0x22, 0x23, 0x26, 0x27):
                    b = "vl::splat(vl::normf(I))"
                else:
                    b = nv(ft)
                if sp <= 0x03 or sp in (0x20, 0x22, 0x28):
                    fam = "Add"
                elif sp <= 0x07 or sp in (0x24, 0x26, 0x2C):
                    fam = "Sub"
                elif sp <= 0x0B or sp in (0x21, 0x23, 0x29):
                    fam = "MAdd"
                elif sp <= 0x0F or sp in (0x25, 0x27, 0x2D):
                    fam = "MSub"
                else:
                    fam = "Mul"
                if dest:
                    commit = fmac_line(fam, nv(fs), b, "vl::norm(acc)" if fam in ("MAdd", "MSub") else "vl::kZero", dest, "acc")
            elif 0x10 <= sp <= 0x13:
                self.vf_used.add(fs)
                out.append(f"    const vl::V {up} = vl::itof<{[0, 4, 12, 15][sp & 3]}>({vf(fs)});")
                commit = (ft, dest)
            elif 0x14 <= sp <= 0x17:
                out.append(f"    const vl::V {up} = vl::ftoi<{[0, 4, 12, 15][sp & 3]}, 0x{dest:x}>({nv(fs)});")
                commit = (ft, dest)
            elif sp == 0x1D:
                out.append(f"    const vl::V {up} = vl::vabs({nv(fs)});")
                commit = (ft, dest)
            elif sp == 0x1F:
                # CLIP: raw bits, queued 4 cycles (queueClip).
                n = len(st.clip)
                out.append(f"    clipW = ((clipW << 6) | vl::clipFlags({vf(fs)}, {vf(ft)})) & 0xFFFFFFu; clipPend{n} = clipW;")
                self.uses.add(f"clipPend{n}")
                st.clip.append(t + FMAC_LATENCY)
                st.pend_max = max(st.pend_max, t + FMAC_LATENCY)
            elif sp in (0x2F, 0x30):
                pass
            else:
                raise LiftError(f"0x{p.pc:04x}: upper special {sp:#x} not supported")
        if p.is_fmac_flag_op():
            st.pend_max = max(st.pend_max, t + FMAC_LATENCY)
        if commit is not None and commit[0] == 0:
            commit = None   # writes to VF00 are discarded
        if commit is not None and commit[1] == 0:
            commit = None

        out += self.lower_sem(p, st, t)

        if commit is not None:
            target, d = commit
            reg = "acc" if target == "acc" else vf(target)
            if d == 0xF:
                out.append(f"    {reg} = {up};")
            else:
                out.append(f"    {reg} = vl::blend<0x{blend_imm(d):x}>({reg}, {up});")
        if p.ibit:
            out.append(f"    I = {fbits(normalize_bits(p.lower))};")
        return out

    def lower_sem(self, p, st, t):
        out = []
        w = p.lower
        k = p.lower_kind()
        dest = (w >> 21) & 0xF
        vft, vfs = (w >> 16) & 31, (w >> 11) & 31
        vit, vis, vid = (w >> 16) & 15, (w >> 11) & 15, (w >> 6) & 15
        imm11 = s11(w)
        drop_vf = p.suppressed_lower_vf  # upper writes the same VF: lower's write is lost
        for r in (vit, vis, vid):
            if r:
                self.vi_used.add(r)

        def wvf(reg, expr):
            if reg == 0 or reg == drop_vf or dest == 0:
                return
            self.vf_used.add(reg)
            if dest == 0xF:
                out.append(f"    {vf(reg)} = {expr};")
            else:
                out.append(f"    {vf(reg)} = vl::blend<0x{blend_imm(dest):x}>({vf(reg)}, {expr});")

        def wvi(reg, expr):
            if reg:
                out.append(f"    {vi(reg)} = {expr};")

        def store_cycle():
            return f"T + {t}" if st.kick else "0"

        if k in ("nop", "loi", "waitq", "waitp"):
            return out
        if k == "lq":
            wvf(vft, f"vl::lq(mem, {vi(vis)} + {imm11})")
        elif k == "sq":
            self.vf_used.add(vfs)
            if dest:
                out.append(f"    vl::sq<0x{blend_imm(dest):x}, {str(st.kick).lower()}>(c, mem, {vi(vit)} + {imm11}, {vf(vfs)}, {store_cycle()});")
            st.pend_max = max(st.pend_max, t + 1)
        elif k == "ilw":
            comp = next((c for c in range(4) if dest & lane(c)), 3)
            wvi(vit, f"vl::ilw(mem, {vi(vis)} + {imm11}, {comp})")
        elif k == "isw":
            if dest:
                out.append(f"    vl::isw<0x{blend_imm(dest):x}, {str(st.kick).lower()}>(c, mem, {vi(vis)} + {imm11}, {vi(vit)}, {store_cycle()});")
            st.pend_max = max(st.pend_max, t + 1)
        elif k in ("iaddiu", "isubiu"):
            imm = (w & 0x7FF) | ((w >> 10) & 0x7800)
            wvi(vit, f"vl::i16({vi(vis)} {'+' if k == 'iaddiu' else '-'} {imm})")
        elif k in ("iadd", "isub"):
            wvi(vid, f"vl::i16({vi(vis)} {'+' if k == 'iadd' else '-'} {vi(vit)})")
        elif k == "iaddi":
            imm5 = ((w >> 6) & 0x1F)
            imm5 = imm5 - 32 if imm5 & 0x10 else imm5
            wvi(vit, f"vl::i16({vi(vis)} + {imm5})")
        elif k == "iand":
            wvi(vid, f"{vi(vis)} & {vi(vit)}")
        elif k == "ior":
            wvi(vid, f"{vi(vis)} | {vi(vit)}")
        elif k == "fcand":
            self.vi_used.add(1)
            out.append(f"    vi01 = (clip & 0x{w & 0xFFFFFF:06x}u) != 0u ? 1 : 0;")
        elif k == "fceq":
            self.vi_used.add(1)
            out.append(f"    vi01 = (clip & 0xFFFFFFu) == 0x{w & 0xFFFFFF:06x}u ? 1 : 0;")
        elif k == "fcor":
            self.vi_used.add(1)
            out.append(f"    vi01 = ((clip | 0x{w & 0xFFFFFF:06x}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1 : 0;")
        elif k == "fcget":
            wvi(vit, "static_cast<int32_t>(clip & 0x0FFFu)")
        elif k == "fcset":
            # queueFcset; a CLIP issued in the same cycle loses its clip write.
            if p.uu.writes_clip:
                raise LiftError(f"0x{p.pc:04x}: CLIP and FCSET in one pair")
            n = len(st.clip)
            out.append(f"    clipW = 0x{w & 0xFFFFFF:06x}u; clipPend{n} = clipW;")
            self.uses.add(f"clipPend{n}")
            st.clip.append(t + FMAC_LATENCY)
            st.pend_max = max(st.pend_max, t + FMAC_LATENCY)
        elif k in ("b", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"):
            def br(r):
                if r == 0:
                    return "0"
                return "viBk" if st.bk_reg == r else f"vl::i16({vi(r)})"
            cond = {"b": "true",
                    "ibeq": f"{br(vis)} == {br(vit)}", "ibne": f"{br(vis)} != {br(vit)}",
                    "ibltz": f"{br(vis)} < 0", "ibgtz": f"{br(vis)} > 0",
                    "iblez": f"{br(vis)} <= 0", "ibgez": f"{br(vis)} >= 0"}[k]
            if k != "b":
                out.append(f"    const bool taken_{p.pc:04x} = {cond};")
        elif k in ("lqi", "lqd"):
            if k == "lqd":
                wvi(vis, f"vl::i16({vi(vis)} - 1)")
            wvf(vft, f"vl::lq(mem, vl::u16({vi(vis)}))")
            if k == "lqi":
                wvi(vis, f"vl::i16({vi(vis)} + 1)")
        elif k in ("sqi", "sqd"):
            self.vf_used.add(vfs)
            if k == "sqd":
                wvi(vit, f"vl::i16({vi(vit)} - 1)")
            if dest:
                out.append(f"    vl::sq<0x{blend_imm(dest):x}, {str(st.kick).lower()}>(c, mem, vl::u16({vi(vit)}), {vf(vfs)}, {store_cycle()});")
            if k == "sqi":
                wvi(vit, f"vl::i16({vi(vit)} + 1)")
            st.pend_max = max(st.pend_max, t + 1)
        elif k == "div":
            fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
            self.vf_used.update(r for r in (vfs, vft) if r)
            out.append(f"    qPend = vl::div(vl::lane<{fsf}>({vf(vfs)}), vl::lane<{ftf}>({vf(vft)}), diPend);")
            st.fdiv = t + 7
            st.pend_max = max(st.pend_max, t + 7)
        elif k == "mtir":
            comp = (w >> 21) & 3
            self.vf_used.add(vfs)
            wvi(vit, f"vl::i16(static_cast<int32_t>(vl::bits<{comp}>({vf(vfs)})))")
        elif k == "mfir":
            wvf(vft, f"vl::splatBits(vl::i16({vi(vis)}))")
        elif k == "move":
            self.vf_used.add(vfs)
            wvf(vft, vf(vfs))
        elif k == "mr32":
            self.vf_used.add(vfs)
            wvf(vft, f"vl::mr32({vf(vfs)})")
        elif k == "mfp":
            wvf(vft, "vl::splat(p)")
        elif k == "xtop":
            wvi(vit, "static_cast<int32_t>(top & 0x3FFu)")
        elif k == "xitop":
            wvi(vit, "static_cast<int32_t>(itop & 0x3FFu)")
        elif k == "xgkick":
            out.append(f"    vl::kickStart(c, vl::u16({vi(vis)}), T + {t});")
            st.kick = True
        elif k == "efu":
            sp = (w & 3) | ((w >> 4) & 0x7C)
            if sp != 0x70:
                raise LiftError(f"0x{p.pc:04x}: EFU op {sp:#x} not supported yet")
            self.vf_used.add(vfs)
            n = len(st.efu)
            if n >= 2:
                raise LiftError(f"0x{p.pc:04x}: more than two EFU results pending")
            out.append(f"    pPend{n} = vl::esadd({vf(vfs)});")
            lat = EFU_LATENCY[sp]
            st.efu.append(t + lat)
            st.efu_res = t + lat - 1
            out.append(f"    efuReady = T + {t + lat - 1};")
            st.pend_max = max(st.pend_max, t + lat)
        else:
            raise LiftError(f"0x{p.pc:04x}: lower op {k} not supported")
        if p.first_vi_write and p.lu.delays_branch:
            # A branch in the next pair reads the pre-write value (readBranchVi).
            r = p.first_vi_write
            out.insert(0, f"    viBk = vl::i16({vi(r)});")
        return out

    # --- output -------------------------------------------------------------
    def emit(self, fn_name):
        # Leaders: CFG block starts, so fallthrough into a shared block ends
        # the current one.
        _, blocks, _ = vu1dis.build_cfg(self.img, self.segment)
        blocks = vu1dis.split_blocks(blocks)
        self.leaders = set(blocks)
        for leader, (end, succs, note) in blocks.items():
            self.leaders.update(succs)
        bodies = self.lift()
        L = []
        L.append(f"static void {fn_name}(vl::Run &c)")
        L.append("{")
        L.append("    uint8_t *const mem = c.mem;")
        L.append("    VU1State &s = c.st;")
        for r in sorted(self.vf_used - {0}):
            L.append(f"    vl::V vf{r:02d} = vl::load(s.vf[{r}]);")
        for r in sorted(self.vi_used - {0}):
            L.append(f"    int32_t vi{r:02d} = s.vi[{r}];")
        L.append("    vl::V acc = vl::load(s.acc);")
        L.append("    float q = s.q, p = s.p, I = s.i;")
        L.append("    float qPend = 0.0f, pPend0 = 0.0f, pPend1 = 0.0f;")
        L.append("    uint32_t diPend = 0u, statusDi = s.status & 0x30u;")
        L.append("    uint32_t clip = s.clip, clipW = s.clip;")
        L.append("    uint32_t clipPend0 = 0u, clipPend1 = 0u, clipPend2 = 0u, clipPend3 = 0u, clipPend4 = 0u;")
        L.append("    vl::Flags fl{s.status & 0xFu, s.mac, s.status & 0xFC0u};")
        L.append("    const uint32_t top = s.top, itop = s.itop;")
        L.append("    int32_t viBk = 0;")
        L.append("    uint64_t T = c.cycle, H = c.cycle, efuReady = c.efuReady;")
        L.append("    uint32_t endPc = 0u;")
        L.append("    (void)top; (void)itop; (void)viBk; (void)pPend0; (void)pPend1; (void)qPend; (void)diPend;")
        L.append("    (void)clipW; (void)clipPend0; (void)clipPend1; (void)clipPend2; (void)clipPend3; (void)clipPend4;")
        L.append(f"    goto {self.entry};")
        for name, pc, st in self.order:
            L.append(f"{name}: // 0x{pc:04x}")
            L.append("{")
            L += bodies[name]
            L.append("}")
        L.append("done:")
        for r in sorted(self.vf_used - {0}):
            L.append(f"    vl::store(s.vf[{r}], vf{r:02d});")
        for r in sorted(self.vi_used - {0}):
            L.append(f"    s.vi[{r}] = vi{r:02d};")
        L.append("    vl::store(s.acc, acc);")
        L.append("    s.q = q; s.p = p; s.i = I;")
        L.append("    s.clip = clip;")
        L.append("    s.mac = fl.mac;")
        L.append("    s.status = fl.sticky | statusDi | fl.cur;")
        L.append("    s.pc = endPc;")
        L.append("    c.endCycle = H;")
        L.append("    c.efuReady = efuReady;")
        L.append("}")
        return L


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 1
    image_path = args[0]
    segments, out_path = [], None
    i = 1
    while i < len(args):
        if args[i] == "--segment":
            segments.append(int(args[i + 1], 0))
            i += 2
        elif args[i] == "-o":
            out_path = args[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {args[i]}")
    img = open(image_path, "rb").read()
    if len(img) != 0x4000:
        raise SystemExit("expected a 16 KB VU1 micro memory image")
    h = fnv1a(img)
    L = ["// Generated by scripts/maintainer/vu1lift.py. Embeds game microcode logic:",
         "// never commit this file.",
         f"// Image {h:016x}, segments {', '.join(f'0x{s:04x}' for s in segments)}.",
         "#include \"runtime/ps2_vu1_lift.h\"",
         "",
         "namespace",
         "{"]
    regs = []
    for seg in segments:
        em = Emitter(img, seg)
        name = f"seg_{seg:04x}"
        try:
            body = em.emit(name)
        except LiftError as e:
            raise SystemExit(f"segment 0x{seg:04x}: {e}")
        L += ["    " + l if l else l for l in body]
        L.append("")
        regs.append((seg, name, len(em.order)))
        print(f"segment 0x{seg:04x}: {len(em.order)} block instances", file=sys.stderr)
    L.append("    const vl::Registration kRegistrations[] = {")
    for seg, name, _ in regs:
        L.append(f"        {{0x{h:016x}ull, 0x{seg:04x}u, &{name}}},")
    L.append("    };")
    L.append("    const vl::Registrar kRegistrar(kRegistrations, sizeof(kRegistrations) / sizeof(kRegistrations[0]));")
    L.append("}")
    text = "\n".join(L) + "\n"
    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
