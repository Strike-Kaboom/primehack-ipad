#!/usr/bin/env python3
# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline DOL to freestanding C++ translator. All guest addresses and words are big-endian."""
from __future__ import annotations

import argparse
from functools import cached_property
from collections import Counter, defaultdict, deque
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from bisect import bisect_right
import json
import hashlib
import re
import tempfile
import struct

U32 = 0xffffffff
WORD = struct.Struct(">I")


def signed(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def fnv1a(data: bytes) -> int:
    h = 0xcbf29ce484222325
    for byte in data:
        h = ((h ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return h


@dataclass(frozen=True)
class Section:
    base: int
    data: bytes
    text: bool

    @property
    def end(self):
        return self.base + len(self.data)

    def words(self):
        for offset in range((-self.base) & 3, len(self.data) - 3, 4):
            yield self.base + offset, WORD.unpack_from(self.data, offset)[0]


@dataclass
class Image:
    sections: list[Section]
    entry: int

    @classmethod
    def dol(cls, data: bytes):
        if len(data) < 0x100:
            raise ValueError("truncated DOL header")
        sections = []
        for i in range(18):
            off, addr, size = (WORD.unpack_from(data, p + 4*i)[0] for p in (0, 0x48, 0x90))
            if size == 0:
                continue
            if off < 0x100 or off + size > len(data) or addr + size > 1 << 32:
                raise ValueError(f"invalid DOL section {i}")
            if i < 7 and (addr | size) & 3:
                raise ValueError(f"unaligned text section {i}")
            if any(addr < s.end and s.base < addr + size for s in sections):
                raise ValueError(f"overlapping guest sections in one DOL at {addr:#x}")
            sections.append(Section(addr, data[off:off+size], i < 7))
        return cls(sections, WORD.unpack_from(data, 0xe0)[0])

    def section(self, pc):
        return next((s for s in self.sections if s.base <= pc and pc + 4 <= s.end), None)

    def word(self, pc):
        sec = self.section(pc)
        return WORD.unpack_from(sec.data, pc - sec.base)[0] if sec and not pc & 3 else None

    def executable(self, pc):
        sec = self.section(pc)
        return sec and sec.text and not pc & 3


# PrimeHack for iPad: use the shipped instruction table as the offline timing oracle.
def read_opinfo():
    path = Path(__file__).resolve().parents[2] / "Source/Core/Core/PowerPC/PPCTables.cpp"
    source = re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8"))
    rows = re.findall(r'\{\s*\d+,\s*"([^"]+)",\s*OpType::\w+,\s*(\d+),\s*([^}]+)\}', source)
    info = {name: (int(cycles), frozenset(re.findall(r"FL_\w+", flags)))
            for name, cycles, flags in rows}
    info["unknown"] = info["unknown_instruction"]
    return info


OPINFO = read_opinfo()
PRIMARY = {
    4: "RunTable4", 19: "RunTable19", 31: "RunTable31", 59: "RunTable59", 63: "RunTable63",
    16: "bcx", 18: "bx", 3: "twi", 17: "sc", 7: "mulli", 8: "subfic", 10: "cmpli",
    11: "cmpi", 12: "addic", 13: "addic_rc", 14: "addi", 15: "addis", 20: "rlwimix",
    21: "rlwinmx", 23: "rlwnmx", 24: "ori", 25: "oris", 26: "xori", 27: "xoris",
    28: "andi_rc", 29: "andis_rc", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 40: "lhz",
    41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 36: "stw", 37: "stwu",
    38: "stb", 39: "stbu", 46: "lmw", 47: "stmw", 48: "lfs", 49: "lfsu", 50: "lfd",
    51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu", 56: "psq_l", 57: "psq_lu",
    60: "psq_st", 61: "psq_stu",
}
PS_X = {
    0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 136: "ps_nabs", 264: "ps_abs",
    64: "ps_cmpu1", 72: "ps_mr", 96: "ps_cmpo1", 528: "ps_merge00", 560: "ps_merge01",
    592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l",
}
PS_A = {
    10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
    15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
    25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub",
    31: "ps_nmadd",
}
PS_Q = {
    6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux",
}
XL = {
    528: "bcctrx", 16: "bclrx", 257: "crand", 129: "crandc", 289: "creqv", 225: "crnand",
    33: "crnor", 449: "cror", 417: "crorc", 193: "crxor", 150: "isync", 0: "mcrf", 50: "rfi",
    51: "vmcall",
}
X = {
    266: "addx", 778: "addox", 10: "addcx", 522: "addcox", 138: "addex", 650: "addeox",
    234: "addmex", 746: "addmeox", 202: "addzex", 714: "addzeox", 491: "divwx",
    1003: "divwox", 459: "divwux", 971: "divwuox", 75: "mulhwx", 11: "mulhwux",
    235: "mullwx", 747: "mullwox", 104: "negx", 616: "negox", 40: "subfx", 552: "subfox",
    8: "subfcx", 520: "subfcox", 136: "subfex", 648: "subfeox", 232: "subfmex",
    744: "subfmeox", 200: "subfzex", 712: "subfzeox", 28: "andx", 60: "andcx", 444: "orx",
    124: "norx", 316: "xorx", 412: "orcx", 476: "nandx", 284: "eqvx", 0: "cmp", 32: "cmpl",
    26: "cntlzwx", 922: "extshx", 954: "extsbx", 536: "srwx", 792: "srawx", 824: "srawix",
    24: "slwx", 54: "dcbst", 86: "dcbf", 246: "dcbtst", 278: "dcbt", 470: "dcbi",
    758: "dcba", 1014: "dcbz", 23: "lwzx", 55: "lwzux", 279: "lhzx", 311: "lhzux",
    343: "lhax", 375: "lhaux", 87: "lbzx", 119: "lbzux", 534: "lwbrx", 790: "lhbrx",
    150: "stwcxd", 20: "lwarx", 533: "lswx", 597: "lswi", 151: "stwx", 183: "stwux",
    407: "sthx", 439: "sthux", 215: "stbx", 247: "stbux", 662: "stwbrx", 918: "sthbrx",
    661: "stswx", 725: "stswi", 535: "lfsx", 567: "lfsux", 599: "lfdx", 631: "lfdux",
    663: "stfsx", 695: "stfsux", 727: "stfdx", 759: "stfdux", 983: "stfiwx", 19: "mfcr",
    83: "mfmsr", 144: "mtcrf", 146: "mtmsr", 210: "mtsr", 242: "mtsrin", 339: "mfspr",
    467: "mtspr", 371: "mftb", 512: "mcrxr", 595: "mfsr", 659: "mfsrin", 4: "tw",
    598: "sync", 982: "icbi", 310: "eciwx", 438: "ecowx", 854: "eieio", 306: "tlbie",
    566: "tlbsync",
}
FS = {
    18: "fdivsx", 20: "fsubsx", 21: "faddsx", 24: "fresx", 25: "fmulsx", 28: "fmsubsx",
    29: "fmaddsx", 30: "fnmsubsx", 31: "fnmaddsx",
}
FD_X = {
    264: "fabsx", 32: "fcmpo", 0: "fcmpu", 14: "fctiwx", 15: "fctiwzx", 72: "fmrx",
    136: "fnabsx", 40: "fnegx", 12: "frspx", 64: "mcrfs", 583: "mffsx", 70: "mtfsb0x",
    38: "mtfsb1x", 134: "mtfsfix", 711: "mtfsfx",
}
FD_A = {
    18: "fdivx", 20: "fsubx", 21: "faddx", 23: "fselx", 25: "fmulx", 26: "frsqrtex",
    28: "fmsubx", 29: "fmaddx", 30: "fnmsubx", 31: "fnmaddx",
}


def opcode(w):
    op, xo = w >> 26, w >> 1 & 1023
    if op == 4:
        return PS_X.get(xo) or PS_Q.get(xo & 63) or PS_A.get(xo & 31, "unknown")
    if op == 63:
        return FD_X.get(xo) or FD_A.get(xo & 31, "unknown")
    if op == 59:
        return FS.get(xo & 31, "unknown")
    if op in (19, 31):
        return (XL if op == 19 else X).get(xo, "unknown")
    return PRIMARY.get(op, "unknown")


def destination(pc, w):
    bits = 26 if w >> 26 == 18 else 16
    disp = signed(w & ~3, bits)
    return (disp + (0 if w & 2 else pc)) & U32


def switch_clobbers(w):
    """GPR/CR definitions that can kill a reaching table, index or compare."""
    name = opcode(w)
    flags = OPINFO[name][1]
    d, a = (w >> sh & 31 for sh in (21, 16))
    regs, fields = set(), set()
    if "FL_OUT_A" in flags or "FL_OUT_AD" in flags:
        regs.add(a)
    if "FL_OUT_D" in flags or "FL_OUT_AD" in flags:
        regs.add(d)
    if name == "lmw":
        regs.update(range(d, 32))
    if name in ("lswi", "lswx"):
        regs.update(range(32))
    if "FL_SET_CR0" in flags or ("FL_RC_BIT" in flags and w & 1):
        fields.add(0)
    if "FL_SET_CR1" in flags or ("FL_RC_BIT_F" in flags and w & 1):
        fields.add(1)
    if "FL_SET_CRn" in flags:
        fields.add(w >> 23 & 7)
    if name.startswith("cr"):
        fields.add(d // 4)
    if name == "mtcrf":
        fields.update(i for i in range(8) if w & (1 << (19-i)))
    elif "FL_SET_ALL_CR" in flags:
        fields.update(range(8))
    return regs, fields


def switch_descriptor(image, pc):
    """Follow the reaching definitions of the table address, loaded word and CTR."""
    if image.word(pc) != 0x4e800420:
        return None
    constants, scales, compares, bounds, versions, loads = {}, {}, {}, {}, {}, {}
    ctr = None
    for at in range(max(image.section(pc).base, pc - 96), pc, 4):
        w = image.word(at)
        if w == 0x60000000:  # nop preserves the reaching definition of r0.
            continue
        op, d, a, b = w >> 26, w >> 21 & 31, w >> 16 & 31, w >> 11 & 31
        name = opcode(w)
        value = scale = load = None
        if op == 15 and a == 0:
            value = (w & 0xffff) << 16
        elif op == 14 and a and a in constants:
            value = (constants[a] + signed(w, 16)) & U32
        elif op == 24 and d in constants:
            value = constants[d] | (w & 0xffff)
        elif op == 21 and (w & 0xffff) == 0x103a:
            scale = (d, versions.get(d))
        elif op == 10 and not w & (1 << 21):
            compares[w >> 23 & 7] = ((a, versions.get(a)), w & 0xffff)
            continue
        elif (op == 16 and not w & 1 and (d & 30, a & 3) in ((12, 1), (4, 0))
              and not at < destination(at, w) <= pc):
            if a >> 2 in compares:
                key, limit = compares[a >> 2]
                bounds[key] = limit + int((d & 30) == 12)
            continue
        elif name == "lwzx" and a:
            for base, index in ((a, b), (b, a)):
                if base in constants:
                    load = (constants[base], bounds.get(scales.get(index)), False)
                    break
        elif name == "addx":
            for source, base in ((a, b), (b, a)):
                prior = loads.get(source)
                if prior and not prior[2] and constants.get(base) == prior[0]:
                    load = (prior[0], prior[1], True)
                    break
        elif name == "mtspr" and spr(w) == 9:
            ctr = loads.get(d)
        elif name in BRANCH or ends_region(w):
            for facts in (constants, scales, compares, bounds, versions, loads):
                facts.clear()
            ctr = None
            continue

        regs, fields = switch_clobbers(w)
        for changed in regs:
            constants.pop(changed, None)
            scales.pop(changed, None)
            loads.pop(changed, None)
            versions[changed] = at
        for field in fields:
            compares.pop(field, None)
        if value is not None:
            constants[a if op == 24 else d] = value
        if scale is not None:
            scales[a] = scale
        if load is not None:
            loads[d] = load
    return ctr


def discovery(image):
    proven, guesses, pointers, switches = set(), set(), set(), {}
    if image.executable(image.entry):
        proven.add(image.entry)
    for sec in image.sections:
        for pc, w in sec.words():
            if not sec.text:
                if image.executable(w):
                    pointers.add(w)
            elif w >> 26 in (16, 18) and w & 1 and image.executable(destination(pc, w)):
                proven.add(destination(pc, w))
            elif w >> 16 == 0x9421 and signed(w, 16) < 0:
                start = pc
                for prev in range(pc - 4, max(sec.base - 4, pc - 96), -4):
                    save = image.word(prev)
                    if save == 0x7c0802a6:
                        start = prev
                        break
                    if not ((save >> 26 in (36, 47, 54, 60) and save >> 16 & 31 == 1) or
                            opcode(save) == "mfcr"):
                        break
                guesses.add(start)
            elif w == 0x4e800420:
                desc = switch_descriptor(image, pc)
                if desc:
                    switches[pc] = desc
    tables = {}
    bases = sorted({desc[0] for desc in switches.values()})
    for pc, (addr, length, relative) in switches.items():
        sec = image.section(addr)
        if not sec or addr & 3 or (sec.text and (not relative or length is None)):
            continue
        i = bisect_right(bases, addr)
        stop = min(sec.end, bases[i] if i < len(bases) else sec.end)
        if length is not None:
            if addr + 4 * length > stop:
                continue
            stop = addr + 4 * length
        targets = []
        for p in range(addr, stop - 3, 4):
            target = image.word(p)
            if relative:
                target = (addr + target) & U32
            if not image.executable(target):
                break
            targets.append(target)
        if length is None or len(targets) == length:
            tables[pc] = tuple(targets)
    cases = {p for targets in tables.values() for p in targets}
    return sorted(proven), sorted((guesses | (pointers - cases)) - proven), tables


def discover(image):
    proven, guesses, tables = discovery(image)
    return sorted(proven + guesses), tables


@dataclass
class Region:
    start: int
    data: bytes
    code: dict[int, int]
    entries: set[int]
    successors: dict[int, tuple[int, ...]]

    @cached_property
    def hash64(self):
        return fnv1a(self.data)


def nonreturning_targets(image, roots):
    """Propagate possible returns back to call targets. Unknown exits may return."""
    maybe = set()
    users = defaultdict(list)
    pending = list(roots)
    seen = set()
    while pending:
        pc = pending.pop()
        if pc in seen:
            continue
        seen.add(pc)
        if not image.executable(pc):
            maybe.add(pc)
            continue
        w = image.word(pc)
        name = opcode(w)
        nxt = (pc + 4) & U32
        # Each alternative returns only if all its dependencies can return.
        if name in ("bx", "bcx"):
            dest = destination(pc, w)
            paths = [(dest, nxt)] if w & 1 else [(dest,)]
            if name == "bcx" and (w >> 21 & 20) != 20:
                paths.append((nxt,))
        elif name in BRANCH or ends_region(w):
            maybe.add(pc)
            continue
        else:
            paths = [(nxt,)]
        for path in paths:
            for dep in path:
                users[dep].append((pc, path))
                if dep not in seen:
                    pending.append(dep)
    ready = list(maybe)
    while ready:
        for caller, deps in users.pop(ready.pop(), ()):
            if caller not in maybe and all(dep in maybe for dep in deps):
                maybe.add(caller)
                ready.append(caller)
    return set(roots) - maybe


def regions(image):
    roots, guesses, tables = discovery(image)
    noreturn = nonreturning_targets(image, roots)
    guess_set = set(guesses)
    owned = set()
    result = []
    for start in roots + guesses:
        if start in owned:
            continue
        i = bisect_right(roots, start)
        sec = image.section(start)
        end = min(sec.end, roots[i]) if i < len(roots) else sec.end
        todo, entries, code, edges = [start], {start}, {}, {}
        while todo:
            pc = todo.pop()
            if pc in code or pc in owned or not start <= pc < end:
                continue
            w = image.word(pc)
            name = opcode(w)
            code[pc] = w
            if pc in guess_set:
                entries.add(pc)
            nxt = pc + 4
            if name in ("bx", "bcx", "bclrx", "bcctrx"):
                conditional = name != "bx" and (w >> 21 & 20) != 20
                if w & 1:
                    can_return = conditional or name not in ("bx", "bcx") or destination(pc, w) not in noreturn
                    successors = ([nxt] if can_return and image.executable(nxt)
                                  and opcode(image.word(nxt)) != "unknown" else [])
                elif name in ("bx", "bcx"):
                    successors = [destination(pc, w)]
                else:
                    successors = list(tables.get(pc, ()))
                if conditional and nxt not in successors and not w & 1:
                    successors.append(nxt)
                entries.update(p for p in successors if start <= p < end)
            elif name in ("rfi", "unknown"):
                successors = []
            else:
                successors = [nxt]
                if ends_region(w):
                    entries.add(nxt)
            edges[pc] = tuple(successors)
            todo.extend(successors)
        owned.update(code)
        stop = max(code) + 4
        data = sec.data[start-sec.base:stop-sec.base]
        result.append(Region(start, data, code, entries.intersection(code), edges))
    return sorted(result, key=lambda r: r.start)


def lift_buffer(data: bytes, base: int) -> list[Region]:
    """CPU differential test API. base is the guest virtual address of data[0]."""
    if not data or len(data) % 4 or base & 3 or not 0 <= base <= (1 << 32)-len(data):
        raise ValueError("expected a nonempty, aligned PPC32 instruction buffer")
    return regions(Image([Section(base, bytes(data), True)], base))

MEMORY = {"lwz": "u32", "lbz": "u8", "lhz": "u16", "lha": "u16",
          "stw": "u32", "stb": "u8", "sth": "u16", "lfs": "u32", "lfd": "u64",
          "stfs": "u32", "stfd": "u64", "lwbr": "u32", "lhbr": "u16",
          "stwbr": "u32", "sthbr": "u16", "stfiw": "u32"}
BRANCH = {"bx", "bcx", "bclrx", "bcctrx"}
FP = (set(PS_A.values()) | set(PS_X.values()) | set(FS.values()) |
      set(FD_X.values()) | set(FD_A.values())) - {
          "dcbz_l", "mcrfs", "mffsx", "mtfsb0x", "mtfsb1x", "mtfsfix", "mtfsfx"}
LOGIC = {"andx": "A & B", "andcx": "A & ~B", "orx": "A | B", "orcx": "A | ~B",
         "xorx": "A ^ B", "eqvx": "~(A ^ B)", "nandx": "~(A & B)", "norx": "~(A | B)"}


def spr(w):
    return (w >> 11 & 31) * 32 + (w >> 16 & 31)


def integer(w):
    """Pure integer lowering using the region-wide register locals."""
    op = w >> 26
    n = opcode(w)
    rd, ra, rb = (w >> sh & 31 for sh in (21, 16, 11))
    d, a, b = (f"r{i}" for i in (rd, ra, rb))
    imm, si = w & 65535, signed(w, 16)
    field = w >> 23 & 7
    rec = bool(w & 1)
    dst = d
    lines = []
    if op in (14, 15):
        val = (si << (16 if op == 15 else 0)) & U32
        return [f"{d} = {a if ra else '0u'} + {val:#x}u;"]
    if op == 7:
        return [f"{d} = {a} * {si & U32:#x}u;"]
    if op in (8, 12, 13):
        # Compute CA before an overlapping destination overwrites its source.
        if op == 8:
            lines = [f"s.xer_ca = {si & U32:#x}u >= {a};", f"{d} = {si & U32:#x}u - {a};"]
        else:
            lines = [f"s.xer_ca = {si & U32:#x}u > ~{a};", f"{d} = {a} + {si & U32:#x}u;"]
        rec = op == 13
    elif op in (10, 11) or n in ("cmp", "cmpl"):
        typ = "s32" if op == 11 or n == "cmp" else "u32"
        rhs = str(si if op == 11 else imm) if op != 31 else b
        return [f"cr{field} = Compare({typ}({a}), {typ}({rhs}), s.xer_so_ov);"]
    elif 24 <= op <= 29:
        val = imm << (16 if op & 1 else 0)
        symbol = ("|", "^", "&")[(op-24)//2]
        dst, rec = a, op >= 28
        lines = [f"{a} = {d} {symbol} {val:#x}u;"]
    elif op in (20, 21, 23):
        mb, me = w >> 6 & 31, w >> 1 & 31
        mask = (U32 >> mb) ^ (U32 >> (me+1))
        if mb > me:
            mask ^= U32
        rot = f"__builtin_rotateleft32({d}, {b if op == 23 else rb})"
        keep = f"({a} & {~mask & U32:#x}u) | " if op == 20 else ""
        dst = a
        lines = [f"{a} = {keep}({rot} & {mask:#x}u);"]
    elif n in ("divwx", "divwox", "divwux", "divwuox"):
        unsigned = "wu" in n
        bad = f"{b} == 0" + ("" if unsigned else f" || ({a} == 0x80000000u && {b} == ~0u)")
        invalid = "0u" if unsigned else f"u32(s32({a}) >> 31)"
        divide = f"{a} / {b}" if unsigned else f"u32(s32({a}) / s32({b}))"
        lines = [f"const bool overflow = {bad};", f"{d} = overflow ? {invalid} : {divide};"]
        if w & 0x400:
            lines.append("SetOverflow(s, overflow);")
    elif n in ("negx", "negox"):
        if w & 0x400:
            lines.append(f"SetOverflow(s, {a} == 0x80000000u);")
        lines.append(f"{d} = 0u - {a};")
    elif n in ("srawx", "srawix"):
        amount = f"{rb}u" if n == "srawix" else f"({b} & 63)"
        dst = a
        lines = [f"const u32 shift = {amount};", f"const s32 value = s32({d});",
                 "s.xer_ca = value < 0 && (shift >= 32 || (u32(value) & ((1u << shift) - 1)));",
                 f"{a} = u32(value >> (shift < 32 ? shift : 31));"]
    elif n.startswith("cr") and op == 19:
        bits = [f"((DecodeCR(cr{i//4}) >> {3-i%4}) & 1)" for i in (ra, rb)]
        x, y = bits
        expr = {"crand": f"{x} & {y}", "crandc": f"{x} & ~{y}", "creqv": f"~({x} ^ {y})",
                "crnand": f"~({x} & {y})", "crnor": f"~({x} | {y})", "cror": f"{x} | {y}",
                "crorc": f"{x} | ~{y}", "crxor": f"{x} ^ {y}"}[n]
        mask = 8 >> (rd % 4)
        return [f"const u32 bit = ({expr}) & 1;",
                f"cr{rd//4} = EncodeCR((DecodeCR(cr{rd//4}) & ~{mask}u) | (bit * {mask}));"]
    elif n == "mcrf":
        return [f"cr{field} = EncodeCR(DecodeCR(cr{w >> 18 & 7}));"]
    elif n == "mfcr":
        return [f"{d} = " + " | ".join(f"(DecodeCR(cr{i}) << {28-4*i})" for i in range(8)) + ";"]
    elif n == "mtcrf":
        # ConditionRegister::Set re-encodes all eight fields, even on a partial mask.
        return [f"cr{i} = EncodeCR(" + (f"({d} >> {28-4*i}) & 15" if w & (1 << (19-i))
                                         else f"DecodeCR(cr{i})") + ");" for i in range(8)]
    elif n in LOGIC:
        dst = a
        lines = [f"{a} = " + LOGIC[n].replace("A", d).replace("B", b) + ";"]
    elif n in ("extsbx", "extshx", "cntlzwx", "slwx", "srwx"):
        expr = {"extsbx": f"static_cast<signed char>({d})", "extshx": f"short({d})",
                "cntlzwx": f"{d} ? __builtin_clz({d}) : 32",
                "slwx": f"({b} & 32) ? 0u : {d} << ({b} & 31)",
                "srwx": f"({b} & 32) ? 0u : {d} >> ({b} & 31)"}[n]
        dst, lines = a, [f"{a} = {expr};"]
    elif n in ("mfspr", "mtspr") and spr(w) in (8, 9):
        reg = "ctr" if spr(w) == 9 else "lr"
        return [f"{d} = {reg};" if n == "mfspr" else f"{reg} = {d};"]
    elif n in ("mullwx", "mulhwux", "mulhwx"):
        expr = {"mullwx": f"{a} * {b}", "mulhwux": f"(u64({a}) * {b}) >> 32",
                "mulhwx": f"(s64(s32({a})) * s32({b})) >> 32"}[n]
        lines = [f"{d} = u32({expr});"]
    elif op == 31 and ((w >> 1) & 511) in (8, 10, 40, 136, 138, 200, 202, 232, 234, 266):
        xo = w >> 1 & 511
        sub = xo in (8, 40, 136, 200, 232)
        lhs = f"u32(~{a})" if sub else a
        rhs = "0u" if xo in (200, 202) else "0xffffffffu" if xo in (232, 234) else b
        carry = "s.xer_ca" if xo in (136, 138, 200, 202, 232, 234) else "1u" if sub else "0u"
        lines = [f"const u64 sum = u64({lhs}) + {rhs} + {carry};"]
        if w & 0x400:
            lines += [f"SetOverflow(s, (({lhs} ^ u32(sum)) & ({rhs} ^ u32(sum))) >> 31);"]
        if xo in (232, 234):
            # Interpreter::addmex/subfmex use Helper_Carry(a, carry - 1), including carry == 1.
            lines.append(f"s.xer_ca = (u32(s.xer_ca) - 1u) > ~({lhs});")
        elif xo not in (40, 266):
            lines.append("s.xer_ca = u8(sum >> 32);")
        lines.append(f"{d} = u32(sum);")
    else:
        return None
    if rec:
        lines.append(f"cr0 = RecordCR({dst}, s.xer_so_ov);")
    return lines


def mem_base(n):
    return n.removesuffix("x").removesuffix("u")


def native_kind(w):
    n = opcode(w)
    if n in BRANCH:
        return "branch"
    if n in FP:
        return "fp"
    if n == "mftb":
        return "timebase"
    if n in ("lmw", "stmw", "dcbz") or mem_base(n) in MEMORY or n.startswith("psq_"):
        return "memory"
    if integer(w) is not None:
        return "integer"
    return None


# BATs, SDR1, HID0 (I-cache), HID4 (extended BATs) and the performance-monitor key.
KEY_OR_MAPPING_SPRS = {25, 952, 956, 1008, 1011, *range(528, 544), *range(560, 576)}


def ends_region(w):
    name = opcode(w)
    return name in {"sc", "rfi", "mtmsr", "mtsr", "mtsrin", "tlbie", "icbi", "isync",
                    "vmcall", "unknown"} or (name == "mtspr" and spr(w) in KEY_OR_MAPPING_SPRS)


def memory(w, pc):
    name = opcode(w)
    base = mem_base(name)
    indexed = name.endswith("x") or name == "dcbz"
    update = name.removesuffix("x").endswith("u")
    d, a, b = (w >> shift & 31 for shift in (21, 16, 11))
    psq = name.startswith("psq_")
    store = name.startswith(("st", "psq_st")) or name == "dcbz"
    store_cpp = str(store).lower()
    off = f"r{b}" if indexed else f"{signed(w, 12 if psq else 16) & U32:#x}u"
    ra = f"r{a}" if a or update else "0u"
    lines = [f"const u32 ea = {ra} + {off};"]
    nxt = (pc + 4) & U32
    guards = []
    address = "ea"
    gather = None
    if psq:
        gqr = 912 + (w >> (7 if indexed else 12) & 7)
        single = bool(w & (1 << (10 if indexed else 15)))
        one = str(single).lower()
        lines += [f"const u32 gqr = s.spr[{gqr}];",
                  f"const u32 type = (gqr >> {0 if store else 16}) & 7;",
                  "const u32 width = type == 0 ? 4 : (type & 1) ? 2 : 1;",
                  f"const u32 size = width * {1 if single else 2};"]
        size, align = "size", "(size == 8 ? 0u : width - 1)"
        if not indexed:
            guards.append("(s.spr[920] & 0x80000000u)")
        fast = [f"if (!Quantized<{store_cpp}>(ps[{d}], page, gqr, {one})) goto cold_{pc:08x};"]
        if store and not update and (indexed or a):
            val = f"ConvertToSingleFTZ(ps[{d}].ps0)"
            if not single:
                val = f"(u64({val}) << 32) | ConvertToSingleFTZ(ps[{d}].ps1)"
            gather = ("type == 0", "u32" if single else "u64", val)
    elif name in ("lmw", "stmw"):
        size, align = str(4 * (32-d)), "3"
        guards.append("!(msr & 1)")
        fast = [(f"Write<u32>(page + {4*(i-d)}, r{i});" if store else
                 f"r{i} = Read<u32>(page + {4*(i-d)});") for i in range(d, 32)]
    elif name == "dcbz":
        # ClearDCacheLine performs eight MMU writes for uncached RAM. Keep MMIO and
        # the emulator's low-MEM1 exception policy in the actual interpreter handler.
        guards.extend(("(s.spr[1008] & (1u << 14))", "!(h.low_dcbz_hack && ea - 0x80000000u < 0x8000)"))
        address, size, align = "ea & ~31u", "32", "0"
        fast = ["__builtin_memset(page, 0, 32);"]
    else:
        typ = MEMORY[base]
        size = f"sizeof({typ})"
        align = "3" if typ == "u64" else f"{size} - 1"
        if not store:
            expr = f"Read<{typ}>(page)"
            if base == "lha":
                expr = f"u32(short({expr}))"
            elif base.endswith("br"):
                expr = f"Swap({expr})"
            fast = [f"Fill(ps[{d}], ConvertToDouble({expr}));" if base == "lfs" else
                    f"ps[{d}].ps0 = {expr};" if base == "lfd" else f"r{d} = {expr};"]
        else:
            value = (f"ConvertToSingle(ps[{d}].ps0)" if base == "stfs" else
                     f"{typ}(ps[{d}].ps0)" if base in ("stfd", "stfiw") else f"{typ}(r{d})")
            if base.endswith("br"):
                value = f"Swap({value})"
            fast = [f"Write<{typ}>(page, {value});"]
            if not update and (indexed or a) and base in ("stw", "sth", "stb", "stfs", "stfd"):
                gather = ("true", typ, value)
    if guards:
        lines.append("if (" + " && ".join(guards) + ")")
    lines += ["{",
              f"  if (auto* page = Page(h, msr, {address}, {size}, {align}, {store_cpp}))", "  {"]
    lines += ["    " + line for line in fast]
    if update:
        lines += [f"    r{a} = ea;"]
    lines += [f"    goto done_{pc:08x};", "  }"]
    if gather:
        lines.insert(0, "bool called = false;")
        guard, typ, value = gather
        lines += [f"  if ({guard} && GatherAddress(h, msr, ea, {size}))", "  {",
                  f"    auto burst = [&] {{ called = true; s.pc = {pc:#x}u; s.npc = {nxt:#x}u;",
                  "      spill(); h.check_gather_pipe(h.context); reload(); };",
                  f"    Gather<{typ}>(s, {value}, burst);", f"    if (called) goto check_{pc:08x};", f"    goto done_{pc:08x};", "  }"]
    lines += ["}"]
    if psq:
        lines.append(f"cold_{pc:08x}:")
    lines += [f"host_op({pc:#x}u, {w:#x}u, h.fallback);", f"check_{pc:08x}:;"]
    return lines


def fp_pairs(w):
    flags = OPINFO[opcode(w)][1]
    operands = set()
    for flag in flags:
        if flag.startswith("FL_IN_FLOAT_"):
            operands.update(flag.split("_")[3])
    if flags & {"FL_OUT_FLOAT_D", "FL_INOUT_FLOAT_D"}:
        operands.add("D")
    return {w >> shift & 31 for field, shift in zip("DABC", (21, 16, 11, 6))
            if field in operands}


def writes_fpscr(flags):
    return bool(flags & {"FL_FLOAT_EXCEPTION", "FL_FLOAT_DIV", "FL_SET_FPRF"})


REG_TOKEN = re.compile(r"\b(?:r\d+|cr\d+|ctr|lr|xer_ca|xer_so_ov)\b|ps\[\d+\]")
REG_ASSIGN = re.compile(r"^\s*(?:s\.)?(r\d+|cr\d+|ctr|lr|xer_ca) = ")


def register_effects(w, lines):
    """Architectural local reads, possible writes and guaranteed full definitions."""
    kind = native_kind(w)
    reads, writes, defined = set(), set(), set()
    for line in lines:
        m = REG_ASSIGN.match(line)
        if m:
            writes.add(m[1])
            line = line[m.end():]
        reads.update(set(REG_TOKEN.findall(line)) - defined)
        if m and kind == "integer":
            defined.add(m[1])
        if "SetOverflow(" in line:
            writes.add("xer_so_ov")
            reads.add("xer_so_ov")
        if "--ctr;" in line:
            writes.add("ctr")
    name = opcode(w)
    if kind == "fp":
        if writes_fpscr(OPINFO[name][1]):
            writes.add("env.fpscr")
            reads.add("env.fpscr")
        elif w & 1:
            reads.add("env.fpscr")
        reads.update(f"ps[{i}]" for i in fp_pairs(w))
        writes.add(f"cr{w >> 23 & 7}" if "cmp" in name else f"ps[{w >> 21 & 31}]")
        if w & 1 and "FL_RC_BIT_F" in OPINFO[name][1]:
            writes.add("cr1")
    if name.startswith(("lfs", "lfd", "psq_l")):
        writes.add(f"ps[{w >> 21 & 31}]")
    return reads, writes, defined


def register_name(binding):
    if binding.startswith("cr["):
        return "cr" + binding[3:-1]
    return binding


def call_registers(region, sites):
    """Forward may-dirty and backward live-in sets, including observable side exits."""
    preds = defaultdict(set)
    for pc, targets in region.successors.items():
        for target in targets:
            if target in sites:
                preds[target].add(pc)
    dirty = {pc: set() for pc in sites}
    live = {pc: set() for pc in sites}
    work = deque(sites)
    while work:
        pc = work.popleft()
        _, writes, _, _, flush = sites[pc]
        outgoing = set() if flush else dirty[pc] | writes
        for target in region.successors[pc]:
            if target in dirty and not outgoing <= dirty[target]:
                dirty[target].update(outgoing)
                work.append(target)
    work.extend(reversed(sites))
    while work:
        pc = work.popleft()
        reads, writes, defs, observable, _ = sites[pc]
        needed = dirty[pc] | writes if observable else set()
        for target in region.successors[pc]:
            needed.update(live.get(target, ()))
        incoming = reads | (needed - defs)
        if not incoming <= live[pc]:
            live[pc].update(incoming)
            work.extend(preds[pc])
    return dirty, live


def render(region, symbol, callees=None):
    # PrimeHack for iPad: one function, sparse locals and straight-line accounting at each edge.
    blocks = []
    for pc, word in sorted(region.code.items()):
        if (not blocks or pc in region.entries or pc != blocks[-1][-1][0] + 4 or
                opcode(blocks[-1][-1][1]) in BRANCH or ends_region(blocks[-1][-1][1])):
            blocks.append([])
        blocks[-1].append((pc, word))
    body = []
    pairs, fields, written = set(), set(), set()
    sparse_calls = bool(callees) and any(
        w >> 26 in (16, 18) and w & 1 and destination(pc, w) in callees
        for pc, w in region.code.items())
    uses_fpscr = False
    sites = {}
    call_sites = []
    for block in blocks:
        start = block[0][0]
        cycles = loads = fp = 0
        body.append(f"L_{start:08x}:")
        for pc, w in block:
            name, kind = opcode(w), native_kind(w)
            cost, flags = OPINFO[name]
            cycles += cost
            loads += "FL_LOADSTORE" in flags
            fp += "FL_USE_FPU" in flags
            charge = f"Charge(downcount, features, h, {start:#x}u, {cycles}, {loads}, {fp});"
            nxt = (pc + 4) & U32
            lines = []
            if kind == "integer":
                lines = integer(w)
            elif kind == "fp":
                pairs.update(fp_pairs(w))
                if writes_fpscr(flags):
                    written.add("env.fpscr")
                uses_fpscr |= writes_fpscr(flags) or bool(w & 1)
                if "cmp" in name:
                    fields.add(w >> 23 & 7)
                    written.add(f"cr[{w >> 23 & 7}]")
                else:
                    written.add(f"ps[{w >> 21 & 31}]")
                if w & 1 and "FL_RC_BIT_F" in flags:
                    fields.add(1)
                    written.add("cr[1]")
                lines = [f"{name}(env, FPInst({w:#x}u));"]
            elif kind == "memory":
                lines = memory(w, pc)
                if name.startswith(("lfd", "lfs", "psq_l")):
                    written.add(f"ps[{w >> 21 & 31}]")
            elif kind != "branch":
                handler = "mftb" if kind == "timebase" else "fallback"
                lines = [f"host_op({pc:#x}u, {w:#x}u, h.{handler});"]
            if kind in ("fp", "memory", None, "timebase"):
                checked = ("true" if name in ("tw", "twi") else
                           "(h.checked_fp || h.checked_div)" if "FL_FLOAT_DIV" in flags else
                           "h.checked_fp" if "FL_FLOAT_EXCEPTION" in flags else "false")
                if "FL_ENDBLOCK" in flags and name not in ("tw", "twi"):
                    checked = "false"
                if checked != "false":
                    mask = "0x80" if kind == "fp" else "0x88"
                    lines += [f"if ({checked} && (s.Exceptions & {mask}))", "{", "  spill();",
                              f"  return CheckedFault(s, h, {start:#x}u, {pc:#x}u, {cycles});", "}"]
                if kind == "fp":
                    if flags & {"FL_FLOAT_EXCEPTION", "FL_FLOAT_DIV"}:
                        lines += ["if ((msr & 0x900) && (s.Exceptions & 0x80))", "{",
                                  "  " + charge, f"  return finish({nxt:#x}u, PH_DISPATCH);", "}"]
                else:
                    guard = ("true" if ends_region(w) else
                             f"downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS) || *h.generation != generation || s.npc != {nxt:#x}u")
                    lines += [f"if ({guard})", "{", "  " + charge,
                              "  return finish(s.npc, PH_DISPATCH);", "}"]
                    if kind == "memory":
                        lines.append(f"done_{pc:08x}:;")
            if pc == block[-1][0] and not ends_region(w):
                lines.append(charge)
                if kind == "branch":
                    lines.extend(branch(region, pc, w, callees))
                elif nxt in region.code:
                    lines.append(f"goto L_{nxt:08x};")
                else:
                    lines.append(f"return finish({nxt:#x}u, PH_BRANCH);")
            if sparse_calls:
                effects = register_effects(w, lines)
                unconditional = name == "bx" or (name in BRANCH and w >> 21 & 20 == 20)
                flush = kind in (None, "timebase") or (unconditional and w & 1)
                observable = any(any(token in line for token in ("finish(", "spill();", "host_op("))
                                 for line in lines)
                sites[pc] = (*effects, observable, flush)
                if any("PUBLISH_CALL" in line for line in lines):
                    call_sites.append(pc)
                site = f"SPILL_{pc:08x}"
                lines = [line.replace("spill();", f"{site}();")
                         .replace("finish(", f"finish({site}, ")
                         .replace("host_op(", f"host_op({site}, ") for line in lines]
            body.extend(["{", *("  " + line for line in lines), "}"])
    code = "\n".join(body)
    bindings = {}
    for token in sorted(set(re.findall(r"\b(?:r\d+|ctr|lr)\b", code))):
        bindings[token] = (f"s.gpr[{token[1:]}]" if token.startswith("r") else
                           f"s.spr[{8 if token == 'lr' else 9}]")
    fields.update(map(int, re.findall(r"\bcr(\d+)\b", code)))
    code = re.sub(r"\bcr(\d+)\b", r"cr[\1]", code)
    for field in ("xer_ca", "xer_so_ov"):
        if f"s.{field}" in code or (field == "xer_so_ov" and "SetOverflow" in code):
            bindings[field] = f"s.{field}"
            code = code.replace(f"s.{field}", field)
    code = code.replace("SetOverflow(s,", "SetOverflow(xer_so_ov,")
    written.update(re.findall(r"^\s*(r\d+|cr\[\d+\]|lr|ctr|xer_ca) = ", code, re.MULTILINE))
    if "--ctr;" in code:
        written.add("ctr")
    if "SetOverflow(" in code:
        written.add("xer_so_ov")
    rows = [f"PHU32 {symbol}(PHState* state, const PHAotHost* host, PHU32 index, PHU32 depth)",
            "{", "  using namespace ph;", "  auto& s = *state;", "  PHAotHost h;",
            "  if (s.Exceptions & ~PH_ASYNC_EXCEPTIONS) return PH_DECLINE;"]
    if any("FL_USE_FPU" in OPINFO[opcode(w)][1] for w in region.code.values()):
        rows += ["  if (!(s.msr & (1u << 13))) return PH_DECLINE;"]
    rows += ["  const u64 generation = *host->generation;", "  s32 downcount;", "  u32 msr, features;",
             "  PHPair ps[32];", "  u64 cr[8];", "  FPEnv env{s, h, ps, cr, 0, 0, false};"]
    for key in bindings:
        typ = "u8" if key.startswith("xer_") else "u32"
        rows.append(f"  {typ} {key};")
    pairs.update(map(int, re.findall(r"\bps\[(\d+)\]", code)))
    for i in sorted(pairs):
        bindings[f"ps[{i}]"] = f"s.ps[{i}]"
    for i in sorted(fields):
        bindings[f"cr[{i}]"] = f"s.cr[{i}]"
    if uses_fpscr:
        bindings["env.fpscr"] = "s.fpscr"
    # These SPRs are read-only in lifted memory operations. A host instruction or
    # native guest callee may update them, just as it may change the translation mode.
    memory_sprs = sorted({int(n) for n in re.findall(r"s\.spr\[(\d+)\]", code)}
                         & {920, 1008, *range(912, 920)})
    for reg in memory_sprs:
        rows.append(f"  u32 spr{reg};")
        code = code.replace(f"s.spr[{reg}]", f"spr{reg}")
    rows += ["  auto reload_host = [&] {", "    const PHState* PH_RESTRICT src = state;",
             "    const PHAotHost* PH_RESTRICT config = host;",
             "    h = *config; downcount = src->downcount;",
             "    features = src->feature_flags; msr = env.msr = src->msr;"]
    rows.extend(f"    spr{reg} = src->spr[{reg}];" for reg in memory_sprs)
    if pairs:
        rows.append("    env.fprf_off = h.fprf_updates_off && *static_cast<const bool*>(h.fprf_updates_off);")
    rows += ["  };", "  auto reload = [&] {", "    reload_host();",
             *(f"    {k} = {v};" for k, v in bindings.items()), "  };"]
    if sparse_calls:
        dirty, live = call_registers(region, sites)
        spills = {}
        replacements = {}
        for pc, (_, writes, _, observable, _) in sites.items():
            if not observable:
                continue
            keys = tuple(sorted(dirty[pc] | writes))
            spills.setdefault(keys, f"spill_{len(spills)}")
            replacements[f"SPILL_{pc:08x}"] = spills[keys]
            replacements[f"PUBLISH_CALL_{pc:08x}"] = f"{spills[keys]}()"
        for pc in call_sites:
            resume = live.get((pc + 4) & U32, set())
            replacements[f"RESUME_CALL_{pc:08x}"] = "\n      ".join(
                f"{k} = {v};" for k, v in bindings.items() if register_name(k) in resume).rstrip(";")
        code = re.sub(r"(?:SPILL|PUBLISH_CALL|RESUME_CALL)_[0-9a-f]{8}",
                      lambda m: replacements[m[0]], code)
        extra_param = "auto& spill, "
    else:
        spills = {tuple(register_name(k) for k in written): "spill"}
        extra_param = ""
    for keys, name in spills.items():
        rows += [f"  auto {name} = [&] {{", "    s.downcount = downcount;",
                 *(f"    {v} = {k};" for k, v in bindings.items() if register_name(k) in keys),
                 "  };"]
    rows += [f"  auto finish = [&]({extra_param}u32 target, PHExit reason) -> u32 {{",
             "    spill(); s.pc = s.npc = target;",
             "    return downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS) ? PH_DISPATCH : reason;",
             "  };", f"  auto host_op = [&]({extra_param}u32 pc, u32 word, auto fn) {{",
             "    HostOp(s, h.context, pc, word, fn, spill, reload);", "  };"]
    rows += ["  reload();", "  switch (index)", "  {"]
    rows += [f"  case {(pc-region.start)//4}: goto L_{pc:08x};" for pc in sorted(region.entries)]
    rows += ["  default: return PH_DISPATCH;", "  }", code, "}"]
    return "\n".join(rows)


def branch(region, pc, w, callees):
    name = opcode(w)
    bo, bi = w >> 21 & 31, w >> 16 & 31
    nxt = (pc+4) & U32
    direct = name in ("bx", "bcx")
    lines, tests = [], []
    if name not in ("bx", "bcctrx") and not bo & 4:
        lines.append("--ctr;")
        tests.append("ctr == 0" if bo & 2 else "ctr != 0")
    if name != "bx" and not bo & 16:
        tests.append(f"((DecodeCR(cr{bi//4}) >> {3-bi%4}) & 1) == {int(bool(bo & 8))}")
    if tests:
        fall = (f"goto L_{nxt:08x};" if nxt in region.code else
                f"return finish({nxt:#x}u, PH_BRANCH);")
        lines += ["if (!(" + " && ".join(tests) + "))", "{", "  " + fall, "}"]
    target = destination(pc, w) if direct else None
    expr = f"{target:#x}u" if direct else "target"
    if not direct:
        lines.append(f"const u32 target = {'ctr' if name == 'bcctrx' else 'lr'} & ~3u;")
    if w & 1:
        lines.append(f"lr = {nxt:#x}u;")
        # A unique registry row gives clang a named native call (including across shards).
        # Overlapping DOLs can have several different callees at exactly the same guest PC.
        if direct and callees and target in callees:
            slot, targets = callees[target]
            call = "callee->fn(state, host, callee->entry_index, depth + 1)"
            identity = ""
            if len(targets) == 1:
                symbol, offset = targets[0]
                identity = f" && callee->fn == {symbol} && callee->entry_index == {offset}"
                call = f"{symbol}(state, host, {offset}, depth + 1)"
            resume = f"RESUME_CALL_{pc:08x}; goto L_{nxt:08x};" if nxt in region.code else "return PH_BRANCH;"
            lines += [f"if (downcount > 0 && !(s.Exceptions & ~PH_ASYNC_EXCEPTIONS) && depth < PH_CALL_LIMIT && h.bound && features < PH_FEATURE_KEYS)", "{",
                      f"  const auto* callee = h.bound[{slot - 1}u * PH_FEATURE_KEYS + features];",
                      f"  if (callee && callee->entry_pc == {expr}{identity})",
                      "  {", f"    PUBLISH_CALL_{pc:08x};",
                      f"    s.pc = s.npc = {expr};",
                      f"    const u32 reason = {call};",
                      "    reload_host();",
                      f"    if (reason != PH_RETURN || s.pc != {nxt:#x}u || s.spr[8] != {nxt:#x}u ||",
                      "        downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS) || *h.generation != generation)",
                      "      return PH_DISPATCH;", "    " + resume, "  }", "}"]
    elif direct and target in region.code:
        if target <= pc:
            lines.append(f"if (downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS)) return finish({expr}, PH_DISPATCH);")
        return lines + [f"goto L_{target:08x};"]
    elif not direct and name == "bcctrx":
        cases = sorted(set(region.successors[pc]) & region.code.keys())
        if cases:
            lines += ["if (downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS)) return finish(target, PH_DISPATCH);", "switch (target)", "{"]
            lines += [f"case {v:#x}u: goto L_{v:08x};" for v in cases]
            lines.append("}")
    reason = "PH_RETURN" if name == "bclrx" and not w & 1 else "PH_BRANCH"
    return lines + [f"return finish({expr}, {reason});"]


def profile_pcs(report):
    return [int(m[1], 16) for m in re.finditer(
        r"^\s*\d+\s+0x([\da-fA-F]{8})\s+\d+\s+\d+\b", report, re.MULTILINE)]


def select(all_regions, max_functions=0, report=None):
    if max_functions < 0:
        raise ValueError("negative function limit")
    if report is not None:
        ranks = {}
        for i, pc in enumerate(profile_pcs(report)):
            ranks.setdefault(pc, i)
        ranked = []
        for region in all_regions:
            hits = region.code.keys() & ranks.keys()
            if hits:
                region.entries.update(hits)
                ranked.append((min(ranks[pc] for pc in hits), region))
        all_regions = [r for _, r in sorted(ranked, key=lambda item: item[0])]
    return all_regions[:max_functions or None]


def write_outputs(regions, out_dir, shards=8):
    """Write the C++ module. Only files in aot_sources.txt belong to this generation."""
    if shards < 1:
        raise ValueError("shards must be positive")
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    functions = []
    seen = set()
    for region in regions:
        key = (region.start, region.data, tuple(sorted(region.entries)), tuple(sorted(region.code)),
               tuple(sorted(region.successors.items())))
        if key not in seen:
            functions.append(region)
            seen.add(key)
    if not functions:
        raise ValueError("no selected functions")
    # Hold only one function's C++ in memory. The registry names the inputs to code generation
    # so an interrupted/concurrent generation cannot link different code at the same PC.
    sources = [f"aot_shard_{i:04}.cpp" for i in range(min(shards, len(functions)))]
    rows, stats = [], Counter()
    # Namespace the whole registry layout. A stale caller from another generation must
    # never link against a differently numbered bound table, even if its PPC bytes match.
    layout = hashlib.sha256(Path(__file__).read_bytes())
    layout.update(repr(sorted((n, c, sorted(f)) for n, (c, f) in OPINFO.items())).encode())
    helpers = Path(__file__).resolve().parents[2] / "Source/Core/Core/PowerPC/Aot"
    for header in ("AotHelpers.h", "AotAbi.h"):
        layout.update((helpers / header).read_bytes())
    for r in functions:
        layout.update(struct.pack(">IQ", r.start, r.hash64))
        for values in (sorted(r.entries), sorted(r.code)):
            layout.update(struct.pack(">I", len(values)))
            for value in values:
                layout.update(WORD.pack(value))
        for pc, targets in sorted(r.successors.items()):
            layout.update(struct.pack(">II", pc, len(targets)))
            for target in targets:
                layout.update(struct.pack(">Q", target))
    build_id = layout.hexdigest()[:24]
    symbols = [f"region_{i}_{build_id}" for i in range(len(functions))]
    entry_variants = defaultdict(list)
    direct_pcs = set()
    for r, symbol in zip(functions, symbols):
        for pc in r.entries:
            entry_variants[pc].append((symbol, (pc - r.start) // 4))
        direct_pcs.update(destination(pc, w) for pc, w in r.code.items()
                          if w >> 26 in (16, 18) and w & 1)
    targets = sorted(direct_pcs & entry_variants.keys())
    callees = {pc: (i, entry_variants[pc]) for i, pc in enumerate(targets, start=1)}
    prototype = "PHU32 {}(PHState*, const PHAotHost*, PHU32, PHU32);"
    decls = [prototype.format(symbol) for symbol in symbols]
    with tempfile.TemporaryDirectory(prefix=".lift-", dir=out) as temp:
        stage = Path(temp)
        with ExitStack() as handles:
            streams = [handles.enter_context((stage/n).open("w", encoding="utf-8")) for n in sources]
            for stream in streams:
                stream.write('#include "AotHelpers.h"\n\n')
            for i, r in enumerate(functions):
                sym = symbols[i]
                stream = streams[i % len(streams)]
                direct_targets = {destination(pc, w) for pc, w in r.code.items()
                                  if w >> 26 in (16, 18) and w & 1}
                for target in sorted(direct_targets):
                    variants = entry_variants.get(target, ())
                    if len(variants) == 1:
                        stream.write(prototype.format(variants[0][0]) + "\n")
                stream.write(render(r, sym, callees) + "\n\n")
                flags = int(any("FL_USE_FPU" in OPINFO[opcode(w)][1] for w in r.code.values()))
                for pc in r.entries:
                    rows.append((pc, f"  {{{pc:#x}u, {r.start:#x}u, {len(r.data)//4}, {flags}, "
                                     f"{r.hash64:#018x}ull, {sym}, {(pc-r.start)//4}, {callees[pc][0] if pc in callees else 0}}},"))
                for w in r.code.values():
                    stats[opcode(w), "lifted" if native_kind(w) else "fallback"] += 1
        registry = ['#include "AotAbi.h"', *decls, "static constexpr PHAotEntry entries[] = {",
                    *(row for _, row in sorted(rows)), "};",
                    f"static constexpr PHAotModule module{{PHAbiVersion(), sizeof(entries)/sizeof(*entries), {len(callees)}, entries}};",
                    "#ifdef _MSC_VER", '#pragma comment(linker, "/export:PHAot_GetModule")',
                    "#define PH_EXPORT", "#else",
                    '#define PH_EXPORT __attribute__((visibility("default")))', "#endif",
                    'extern "C" PH_EXPORT const PHAotModule*',
                    "PHAot_GetModule(PHU32 version, const PHAotHost* host)", "{",
                    "  return host && version == module.abi_version ? &module : nullptr;", "}"]
        sources.append("aot_registry.cpp")
        (stage/sources[-1]).write_text("\n".join(registry) + "\n", encoding="utf-8")
        (stage/"aot_sources.txt").write_text("\n".join(sources) + "\n", encoding="utf-8")
        histogram = Counter(len(r.code) for r in functions)
        summary = {"regions": len(functions), "entries": len(rows),
                   "region_length_histogram": dict(sorted(histogram.items())),
                   "opcodes": {n: {k: stats[n, k] for k in ("lifted", "fallback")}
                               for n in sorted({n for n, _ in stats})}}
        (stage/"aot_stats.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        for name in [*sources, "aot_stats.json", "aot_sources.txt"]:
            (stage/name).replace(out/name)
    return len(functions), len(rows), stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dol", type=Path, action="extend", nargs="+", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--max-functions", type=int, default=0)
    parser.add_argument("--profile-report", type=Path)
    parser.add_argument("--shards", type=int, default=8)
    args = parser.parse_args()
    try:
        all_regions = [r for p in args.dol for r in regions(Image.dol(p.read_bytes()))]
        report = args.profile_report.read_text(encoding="utf-8-sig") if args.profile_report else None
        chosen = select(all_regions, args.max_functions, report)
        nf, ne, stats = write_outputs(chosen, args.out, args.shards)
    except (OSError, ValueError) as err:
        parser.exit(1, f"{err}\n")
    totals = Counter()
    for (op, kind), count in sorted(stats.items()):
        print(f"{op:18} {kind:8} {count}")
        totals[kind] += count
    print(f"{nf} functions, {ne} entries, {totals['lifted']} lifted, {totals['fallback']} fallback")


if __name__ == "__main__":
    main()
