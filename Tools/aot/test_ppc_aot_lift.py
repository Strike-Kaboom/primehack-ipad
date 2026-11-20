# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic fixtures only. Run with py -3 Tools/aot/test_ppc_aot_lift.py."""
import json
import os
import re
import shutil
import subprocess
import struct
import tempfile
import unittest
from pathlib import Path

import ppc_aot_lift as lift

BASE = 0x80003100
DATA = 0x80308000
BLR = 0x4e800020
NOP = 0x60000000


def words(*values):
    return struct.pack(f">{len(values)}I", *values)


def dol(text, pointers=(), entry=BASE):
    header = bytearray(0x100)
    for i, (section, addr, off) in enumerate(((text, BASE, 0x100),
                                              (pointers, DATA, 0x100+len(text)*4))):
        slot = 7*i
        for pos, num in ((0, off), (0x48, addr), (0x90, 4*len(section))):
            struct.pack_into(">I", header, pos+4*slot, num)
    struct.pack_into(">I", header, 0xe0, entry)
    return bytes(header) + words(*text) + words(*pointers)


def body(*ops):
    r = lift.lift_buffer(words(*ops), BASE)[0]
    return lift.render(r, "test")


class Translator(unittest.TestCase):
    def test_hash(self):
        self.assertEqual(lift.fnv1a(b""), 0xcbf29ce484222325)
        self.assertEqual(lift.fnv1a(b"foobar"), 0x85944171f73967e8)
        self.assertEqual(lift.fnv1a(words(0x666f6f62, 0x61720000)),
                         lift.fnv1a(b"foobar\0\0"))

    def test_cfg_early_return_and_call(self):
        # bc forward; bl out; early blr; unreachable; addi; b backwards to early blr.
        image = lift.Image.dol(dol([0x41820010, 0x48000015, BLR, NOP, 0x38600001,
                                     0x4bfffff4, BLR]))
        r, callee = lift.regions(image)
        self.assertEqual(r.entries, {BASE, BASE+4, BASE+8, BASE+16})
        self.assertEqual(r.successors[BASE], (BASE+16, BASE+4))
        self.assertNotIn(BASE+12, r.code)
        self.assertEqual(len(r.data), 24)
        self.assertEqual(callee.start, BASE+24)
        self.assertIn("case 4: goto", lift.render(r, "early"))

    def test_overlapping_dols(self):
        first, second = [lift.regions(lift.Image.dol(dol([0x38600000 | i, BLR])))[0]
                         for i in (1, 2)]
        self.assertNotEqual(first.hash64, second.hash64)
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            path = Path(tmp)
            nf, ne, stats = lift.write_outputs([first, second, first], path, 2)
            self.assertEqual((nf, ne), (2, 2))
            registry = (path/"aot_registry.cpp").read_text()
            for r in (first, second):
                self.assertIn(f"{r.hash64:#018x}ull", registry)
            self.assertEqual(registry.count(f"{{{BASE:#x}u,"), 2)
            self.assertEqual(stats["addi", "lifted"], 2)
            summary = json.loads((path/"aot_stats.json").read_text())
            self.assertEqual(summary, {"regions": 2, "entries": 2, "region_length_histogram": {"2": 2},
                                       "opcodes": {"addi": {"lifted": 2, "fallback": 0},
                                                   "bclrx": {"lifted": 2, "fallback": 0}}})

    def test_pointer_and_prologue(self):
        image = lift.Image.dol(dol([BLR, NOP, BLR, 0x7c0802a6, 0x9421fff0, BLR],
                                   [BASE+8, BASE+1, 0xffffffff]))
        self.assertEqual(lift.discover(image)[0], [BASE, BASE+8, BASE+12])

    def test_switch_absolute_table(self):
        # lis r5,0x8031; addi r5,r5,-32768; lwzx r6,r5,r3; mtctr r6; bctr.
        code = [0x3ca08031, 0x38a58000, 0x7cc5182e, 0x7cc903a6, 0x4e800420,
                BLR, NOP, BLR]
        image = lift.Image.dol(dol(code, [BASE+20, BASE+28, 0]))
        r, = lift.regions(image)
        self.assertEqual(r.successors[BASE+16], (BASE+20, BASE+28))
        self.assertEqual(r.entries, {BASE, BASE+20, BASE+28})
        self.assertNotIn(BASE+24, r.code)
        code[3] = 0x7ce903a6  # mtctr r7; unrelated to lwzx
        r = lift.regions(lift.Image.dol(dol(code, [BASE+20, BASE+28])))[0]
        self.assertEqual(r.successors[BASE+16], ())
        self.assertEqual(len(r.data), 20)

    def test_profile_selects_whole_function(self):
        r = lift.lift_buffer(words(NOP, NOP, BLR), BASE)
        report = f"    1  0x{BASE+4:08x}   45   900  20.0  91.2\n        0x12345678: blr"
        selected = lift.select(r, 1, report)
        self.assertEqual(selected, r)
        self.assertIn(BASE+4, selected[0].entries)

    def test_reject_malformed(self):
        for buf in (b"", dol([BLR])[:-1]):
            with self.assertRaises(ValueError):
                lift.Image.dol(buf)
        for data, base in ((b"abc", BASE), (words(NOP), 3), (words(NOP, BLR), 0xfffffffc)):
            with self.assertRaises(ValueError):
                lift.lift_buffer(data, base)

    # PrimeHack for iPad: static CFG, in-region continuation and literal timing.
    def test_integer_overlap_carry_record(self):
        source = body(0x7c632515, 0x54634707, BLR)
        add = source.index("const u64 sum = u64(r3) + r4 + xer_ca;")
        for line in ("SetOverflow(xer_so_ov,", "xer_ca = u8(sum >> 32)", "r3 = u32(sum)",
                     "cr[0] = RecordCR(r3, xer_so_ov)"):
            self.assertGreater(source.index(line), add)
        self.assertIn("__builtin_rotateleft32", source)

    def test_conditional_link(self):
        source = body(0x41820009, BLR, BLR)
        self.assertLess(source.index("if (!("), source.index("lr = 0x80003104u"))
        source = body(0x4e800021)
        self.assertLess(source.index("const u32 target = lr"), source.index("lr = 0x80003104u"))

    def test_loop_budget(self):
        source = body(NOP, 0x4200fffc, BLR)
        self.assertIn("--ctr;", source)
        self.assertIn("Charge(downcount, features, h, 0x80003100u, 2, 0, 0);", source)
        self.assertIn("if (downcount <= 0 || (s.Exceptions & ~PH_ASYNC_EXCEPTIONS)) return finish(0x80003100u", source)
        self.assertNotIn("trace", source)

    def test_memory_and_fpu(self):
        source = body(0x84630004, 0xc8630002, 0xe0238000, 0xec2218fa, BLR)
        self.assertIn("r3 + 0x4u", source)
        self.assertLess(source.index("r3 = ea;"), source.index("goto done_80003100;"))
        self.assertEqual(source.count("!(s.msr & (1u << 13))"), 1)
        self.assertIn("Quantized<false>", source)
        self.assertIn("fmaddsx(env, FPInst(", source)
        self.assertIn("5, 3, 3);", source)

    def test_fallback_and_reentry(self):
        for instruction in (0x7c000124, 0x44000002, 0xfc00058e):
            source = body(instruction, BLR)
            self.assertIn(f"host_op({BASE:#x}u, {instruction:#x}u, h.fallback);", source)
        self.assertIn("case 1:", body(0x7c000124, BLR))
        self.assertIn("host_op(", body(0x4c000064))

    def test_codewarrior_proven_and_heuristic(self):
        prologue = [0x7c0802a6, 0x90010004, 0x93e1fffc, 0xbfc1fff8, 0xdbc1fff0, 0xf3e10ff8, 0x9421ffe0]
        code = [0x48000009, BLR, *prologue, 0x9421fff0, BLR, *prologue, BLR]
        image = lift.Image.dol(dol(code, [BASE+20, BASE+24]))
        regions = lift.regions(image)
        self.assertEqual([r.start for r in regions], [BASE, BASE+8, BASE+44])
        self.assertEqual(len(regions[1].code), 9)

    def test_bounded_adjacent_switches(self):
        # lis/addi, cmplwi r3,1, bgt default, slwi r0,r3,2, lwzx r6,r5,r0, mtctr, bctr
        code = [0x3ca08031, 0x38a58000, 0x28030001, 0x41810014,
                0x5460103a, 0x7cc5002e, 0x7cc903a6, 0x4e800420, BLR, BLR, BLR]
        image = lift.Image.dol(dol(code, [BASE+32, BASE+36, BASE+40]))
        r = lift.regions(image)[0]
        self.assertEqual(r.successors[BASE+28], (BASE+32, BASE+36))
        self.assertIn("switch (target)", lift.render(r, "switch_test"))

    def test_relative_table_and_reordered_scale(self):
        code = [0x28030001, 0x41810020, 0x5460103a, 0x3ca08031, 0x38a58000,
                0x7cc5002e, 0x7cc62a14, 0x7cc903a6, 0x4e800420, BLR, BLR, BLR]
        offsets = [(BASE+i-DATA) & lift.U32 for i in (36, 40, 44)]
        region = lift.regions(lift.Image.dol(dol(code, offsets)))[0]
        self.assertEqual(region.successors[BASE+32], (BASE+36, BASE+40))

    def test_noreturn_and_wrapping_pc(self):
        r = lift.lift_buffer(words(0x48000101, 0xffffffff & (1 << 26), BLR), BASE)[0]
        self.assertEqual(set(r.code), {BASE})
        last = lift.lift_buffer(words(0xec2218fa), 0xfffffffc)[0]
        self.assertNotIn("0x100000000u", lift.render(last, "last"))
        image = lift.Image([lift.Section(0xfffffffc, words(NOP), True),
                            lift.Section(0, words(0x48000000), True)], 0xfffffffc)
        self.assertEqual(lift.nonreturning_targets(image, {0xfffffffc}), {0xfffffffc})

    def test_noreturn_with_decodable_data(self):
        # wrapper calls a closed loop; both calls have no normal return path.
        code = [0x48000011, 0x3f800000, 0x38600005, BLR,
                0x48000009, BLR, 0x48000000]
        caller, wrapper, loop = lift.lift_buffer(words(*code), BASE)
        self.assertEqual([len(r.code) for r in (caller, wrapper, loop)], [1, 1, 1])
        code[0] = 0x41820011  # conditional call can fall through
        self.assertIn(BASE+4, lift.lift_buffer(words(*code), BASE)[0].code)
        code[-1] = BLR
        self.assertIn(BASE+4, lift.lift_buffer(words(*code), BASE)[0].code)

    def test_switch_clobbers(self):
        code, table = relative_switch()
        def desc():
            return lift.switch_descriptor(lift.Image([lift.Section(0x4100, words(*code), True),
                                                      table], 0x4100), 0x412c)
        self.assertEqual(desc(), (table.base, 2, True))
        code[9:9] = [0x60e70000]  # unrelated ori between table load and add
        code[11:11] = [0x39400000]  # unrelated li before mtctr
        code[13:13] = [0x60000000]  # and again between mtctr and bctr
        image = lift.Image([lift.Section(0x4100, words(*code), True), table], 0x4100)
        self.assertEqual(lift.switch_descriptor(image, 0x4138), (table.base, 2, True))
        code, table = relative_switch()
        code[3] = 0x38600000  # li r3,0 between compare and scale
        self.assertEqual(desc(), (table.base, None, True))
        code[3] = 0x28080000  # overwrite the compared CR field
        self.assertEqual(desc(), (table.base, None, True))
        code[6] = 0x38a00000  # li r5,0 overwrites the table base
        self.assertIsNone(desc())

    def test_switch_nop_preserves_r0_scale(self):
        code, table = relative_switch()
        code.insert(6, NOP)  # between slwi r0,r3,2 and lwzx r6,r5,r0
        image = lift.Image([lift.Section(0x4100, words(*code), True), table], 0x4100)
        self.assertEqual(lift.switch_descriptor(image, 0x4130), (table.base, 2, True))
        code[6] = 0x38000000  # li r0,0 really does kill the scale
        image = lift.Image([lift.Section(0x4100, words(*code), True), table], 0x4100)
        self.assertEqual(lift.switch_descriptor(image, 0x4130), (table.base, None, True))

    def test_downcount_after_memory_callback(self):
        source = body(0x90640000, 0x38a50001, BLR)  # stw r3,0(r4); addi r5,r5,1
        callbacks, rest = source.split("check_80003100:;", 1)
        guard, _ = rest.split("done_80003100:;", 1)
        self.assertIn("if (called) goto check_80003100;", callbacks)
        self.assertIn("host_op(0x80003100u, 0x90640000u, h.fallback);", callbacks)
        self.assertNotIn("downcount <= 0", callbacks.split("if (auto* page", 1)[1])
        self.assertIn("if (downcount <= 0 ||", guard)
        self.assertIn("return finish(s.npc, PH_DISPATCH);", guard)

    def test_fp_operands_and_fast_path(self):
        for word, expected in ((0xfc205890, {1, 11}), (0xfc802840, {0, 5}),
                               (0x10221c20, {1, 2, 3}), (0xec22182a, {1, 2, 3})):
            self.assertEqual(lift.fp_pairs(word), expected)
        code = body(0x84630004, 0x38840001, BLR)
        hot = code.split("if (auto* page", 1)[1].split("goto done_", 1)[0]
        self.assertNotIn("Exceptions", hot)
        self.assertLess(code.index("s.npc != "), code.index("done_80003100:;"))
        self.assertNotIn("CheckedFault(s, h", body(0x7c000124, BLR))
        self.assertIn("GatherAddress", body(0x7c60212e, BLR))  # stwx r3,0,r4

    def test_sparse_native_call(self):
        # r6 is dirty at the call but overwritten before use on return; r7 is live.
        region = lift.lift_buffer(words(0x38c00001, 0x48000101, 0x38c00002,
                                        0x38e70003, BLR), BASE)[0]
        source = lift.render(region, "caller", {BASE+260: (1, [("callee_fn", 0)])})
        publish = source.split("if (callee &&", 1)[1].split("const u32 reason", 1)[0]
        resume = source.split("const u32 reason", 1)[1].split("goto L_", 1)[0]
        name = re.search(r"(spill_\d+)\(\);", publish)[1]
        publish = source.split(f"auto {name} = [&] {{", 1)[1].split("};", 1)[0]
        self.assertIn("s.gpr[6] = r6;", publish)
        self.assertNotIn("s.gpr[7] = r7;", publish)
        self.assertIn("r7 = s.gpr[7];", resume)
        self.assertNotIn("r6 = s.gpr[6];", resume)

    def test_call_return_guards(self):
        region = lift.lift_buffer(words(0x48000101, NOP, BLR), BASE)[0]
        source = lift.render(region, "caller", {BASE+256: (1, [("callee_fn", 0)])})
        for required in ("depth < PH_CALL_LIMIT", "reason != PH_RETURN", "downcount <= 0",
                         "*h.generation != generation", "PH_FEATURE_KEYS + features"):
            self.assertIn(required, source)


def relative_switch():
    # cmplwi/bge, unrelated mr/li/lwz, scale, signed offsets in a text section.
    code = [0x3ca00000, 0x38a54160, 0x28030002, 0x7d094378, 0x40800030, 0x5460103a,
            0x39400009, 0x81610000, 0x7cc5002e, 0x7cc62a14, 0x7cc903a6, 0x4e800420,
            0x38e00005, BLR, 0x38e00006, BLR, 0x38e00007, BLR]
    table = lift.Section(0x4160, words(0xffffffd0, 0xffffffd8, 0xffffffe0), True)
    return code, table


def native_fixture(path):
    programs = {
        0x1000: [0x38600003, 0x3883ffff, 0x7c832215, BLR],
        0x1100: [0x38600007, 0xec22182a, BLR],
        0x1200: [(4 << 26) | (1 << 21) | (2 << 16) | (3 << 11) | (4 << 6) | (29 << 1), BLR],
        0x1210: [0xfc22182a, 0xfc800890, BLR],
        0x1300: [(57 << 26) | (1 << 21) | (3 << 16) | 4, BLR],
        0x1400: [0x38600005, 0x7c000124],
        0x1500: [0x38630001, 0x4200fffc, BLR],
        0x1700: [0x48000101, 0x38e00019, BLR],
        0x1800: [0x38c00011, BLR],
        0x1900: [0x38600001, 0x48000014, NOP, NOP, NOP, NOP, 0x38a0000c, BLR],
        0x1a00: [0xd8230000, BLR],  # stfd f1,0(r3)
        0x1b00: [0x7c640026, BLR],  # mfcr r3
        0x1c00: [0x7c6327d7, BLR],  # divwo. r3,r3,r4
        0x1d00: [0x84630004, BLR],  # lwzu r3,4(r3)
        0x1e00: [0x7c0000ac, 0x38630001, BLR],  # dcbf r0,r0, addi, blr
        0x1600: [(31 << 26) | (3 << 21) | (3 << 16) | (234 << 1), BLR],
    }
    programs.update({
        0x2000: [(60 << 26) | (1 << 21) | (3 << 16), BLR],
        0x2100: [0x48000101, 0x38e0002a, BLR],
        0x2200: [0x7c0000ac, BLR],
        0x2300: [0x48000101, 0x38e0002a, BLR],
        0x2400: [0x7c6803a6, BLR],  # mtlr r3
        0x2500: [0x48000101, 0x38e0002a, BLR],
        0x2600: [0x4bffff04],  # b to the expected return PC is an ordinary exit
        0x2700: [0x7c632630, BLR],  # sraw r3,r3,r4
        0x2800: [0xbbc30000, 0xbfc30008, BLR],  # lmw/stmw r30
        0x2900: [0x7c63242c, 0x7c63252c, BLR],  # lwbrx/stwbrx
        0x2a00: [0x7c0327ec, BLR],  # dcbz r3,r4
        0x2b00: [0x7c6c42e6, BLR],  # mftb r3
        0x3100: [(31 << 26) | (3 << 21) | (3 << 16) | (4 << 11) | (971 << 1) | 1, BLR],
        0x3200: [(31 << 26) | (3 << 21) | (3 << 16) | (616 << 1) | 1, BLR],
        0x2e00: [0x7c680120, 0x7ca00026, BLR],  # mtcrf 0x80,r3; mfcr r5
        0x2c00: [0x4182000c, 0x38600001, 0x48000008, 0x38600002, 0x38830005, BLR],
    })
    programs[0x4700] = [(31 << 26) | (4 << 21) | (3 << 11) | (151 << 1), BLR]
    programs[0x4500] = [0x38a00001, 0x38c00002, 0x480000f9, 0x38c00005, 0x39070003, BLR]
    programs[0x4600] = [0x38850003, 0x38a00009, 0x38e00011, BLR]
    programs[0x4300] = [0x48000101, 0x38e00019, BLR]
    programs[0x4400] = [0x4d820020, 0x38c0000b, BLR]  # beqlr; li r6,11; blr
    for indexed in range(2):
        for store in range(2):
            for single in range(2):
                pc = 0x5000 + 0x100*indexed + 0x20*store + 8*single
                if indexed:
                    # psq_lx/stx f1,0,r3,W,0
                    op = (4 << 26) | (1 << 21) | (3 << 11) | (single << 10) | ((6+store) << 1)
                else:
                    op = ((60 if store else 56) << 26) | (1 << 21) | (3 << 16) | (single << 15)
                programs[pc] = [op, BLR]
    cr_ops = (257, 129, 289, 225, 33, 449, 417, 193)
    for i, xo in enumerate(cr_ops):
        programs[0x3000+i*16] = [(19 << 26) | (1 << 21) | (8 << 16) | (20 << 11) | (xo << 1),
                                  0x7ca00026, BLR]
    # Compile each lifted opcode family using real decoder words, including OE/Rc variants.
    candidates = [op << 26 for op in lift.PRIMARY if op not in (4, 19, 31, 59, 63)]
    for primary, table in [(4, lift.PS_X), (4, lift.PS_A), (4, lift.PS_Q), (19, lift.XL),
                           (31, lift.X), (59, lift.FS), (63, lift.FD_X), (63, lift.FD_A)]:
        candidates.extend((primary << 26) | (xo << 1) for xo in table)
    for i, word in enumerate(candidates):
        if lift.native_kind(word) and lift.opcode(word) not in lift.BRANCH:
            programs[0x10000 + 16*i] = [word, BLR]
    regions = [r for pc, code in programs.items() for r in lift.lift_buffer(words(*code), pc)]
    switch = [0x3ca00000, 0x38a50040, 0x28030001, 0x41810024, 0x5460103a,
              0x7cc5002e, 0x7cc903a6, 0x4e800420, 0x38e00001, BLR, 0x38e00002, BLR, BLR]
    image = lift.Image([lift.Section(0x4000, words(*switch), True),
                        lift.Section(0x40, words(0x4020, 0x4028), False)], 0x4000)
    regions.extend(lift.regions(image))
    code, table = relative_switch()
    section = lift.Section(0x4100, words(*code).ljust(table.base-0x4100, b"\0") + table.data, True)
    region, = lift.regions(lift.Image([section], 0x4100))
    assert region.successors[0x412c] == (0x4130, 0x4138)
    regions.append(region)
    guess = lift.Image([lift.Section(0x4200, words(0x38e00009, 0x38e70002, BLR), True),
                        lift.Section(0x200, words(0x4204), False)], 0x4200)
    regions.extend(lift.regions(guess))
    lift.write_outputs(regions, path, 2)
    (path/"smoke.cpp").write_text(NATIVE_SMOKE, encoding="utf-8")


NATIVE_SMOKE = r"""#include <cstdlib>
#include <bit>
#include <cstdio>
#include <initializer_list>
#include "AotAbi.h"
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "failure at %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static PHState s;
static bool off;
static PHU32 reads, fallback_pc, fault_pc;
static bool fault, invalidate, force_check;
static PHU64 generation = 1;
static const PHAotEntry* bound[256 * PH_FEATURE_KEYS];
static unsigned bursts;
static PHU32 expected_fifo_high = 0x11223344;
static PHU32 pm_cycles, pm_ls, pm_fp;
static PHAotHost h{};
static const PHAotModule* module;

static PHU32 run(PHU32 pc, PHU32 depth = 0)
{
  s.pc = pc;
  for (PHU32 i = 0; i < module->entry_count; ++i)
  {
    auto& e = module->entries[i];
    if (e.entry_pc == pc)
    {
      return e.fn(&s, &h, e.entry_index, depth);
    }
  }
  CHECK(false);
  return 0;
}
void check_psq();
int main()
{
  h.fprf_updates_off = &off;
  h.check_exceptions = [](void*) { fault_pc = s.pc; };
  h.bound = bound;
  h.generation = &generation;
  h.neon = 1;
  h.performance_monitor = [](void*, PHU32 c, PHU32 l, PHU32 f) {
    pm_cycles += c; pm_ls += l; pm_fp += f;
  };
  h.check_gather_pipe = [](void*) {
    CHECK(s.gather_pipe_ptr - s.gather_pipe_base_ptr == 32);
    CHECK(s.gather_pipe_ptr[-4] == (expected_fifo_high >> 24) && s.gather_pipe_ptr[-1] == (expected_fifo_high & 255));
    ++bursts;
    s.gather_pipe_ptr = s.gather_pipe_base_ptr;
    if (force_check) s.downcount = 0;
  };
  h.fallback = [](void*, PHU32 word) {
    fallback_pc = s.pc;
    CHECK(s.npc == s.pc + 4);
    if (force_check) s.downcount = 0;
    if (invalidate) ++generation;
    if (s.pc == 0x1300)
    {
      ++reads;
      if (fault) s.Exceptions |= 8;
      else
      {
        s.gpr[3] += 4;
        s.ps[1] = {0x3ff0000000000000ull, 0x8000000000000000ull};
      }
    }
    else if (s.pc == 0x1e00 || s.pc == 0x2200)
      s.gpr[3] += 6;
    else
    {
      CHECK(word == 0x7c000124);
      if (s.msr & (1u << 14))
        s.Exceptions |= 0x80;
      else
        s.npc = 0x2400;
    }
  };
  CHECK(!PHAot_GetModule(PHAbiVersion()+1, &h) && !PHAot_GetModule(PHAbiVersion(), nullptr));
  module = PHAot_GetModule(PHAbiVersion(), &h);
  CHECK(module && module->abi_version == PHAbiVersion() && module->call_slots <= 256);

  s.spr[8] = 0xabc3;
  s.downcount = 20;
  run(0x1000);
  CHECK(s.gpr[3] == 3 && s.gpr[4] == 5 && s.cr[0] == 5);
  CHECK(s.pc == 0xabc0 && s.npc == s.pc && s.downcount == 16);

  s.downcount = 10;
  run(0x1100);
  CHECK(s.gpr[3] == 3 && !s.Exceptions && s.pc == 0x1100 && s.downcount == 10);

  s = {}; s.msr = 1u << 13; s.spr[920] = 0x80000000;  // LSQE on, PSE off, as permitted by the shipped CIR handlers
  s.downcount = 30;
  const auto set_pair = [](int r, PHU32 bits) {
    const auto d = std::bit_cast<PHU64>(double(std::bit_cast<float>(bits)));
    s.ps[r] = {d,d};
  };
  set_pair(2, 0x42480000); set_pair(3, 0x1b1c72a0); set_pair(4, 0xbc88cc38);
  run(0x1200);
  const auto expected = std::bit_cast<PHU64>(double(std::bit_cast<float>(0xbf55bf17u)));
  CHECK(s.ps[1].ps0 == expected && s.ps[1].ps1 == expected && (s.fpscr >> 12 & 31) == 8);

  // Consecutive dependent FP ops, SNaN priority, quiet payload, untouched scalar PS1.
  s.ps[2].ps0 = 0x7ff0000000000042ull; s.ps[3].ps0 = 0;
  off = true; s.fpscr = 0x14000; s.ps[1].ps1 = 0x1234;
  run(0x1210);
  CHECK(s.ps[1].ps0 == 0x7ff8000000000042ull && s.ps[1].ps1 == 0x1234);
  CHECK(s.ps[4].ps0 == s.ps[1].ps0 && s.fpscr == 0xa1014000);

  s.gpr[3] = 0x20; run(0x1300);
  CHECK(s.pc == s.spr[8] && reads == 1 && s.gpr[3] == 0x24);
  CHECK(s.ps[1].ps0 == 0x3ff0000000000000ull && s.ps[1].ps1 == 0x8000000000000000ull);
  fault = true; s.gpr[3] = 0x20; s.ps[1] = {42, 24}; run(0x1300);
  CHECK(s.Exceptions == 8 && s.ps[1].ps0 == 42 && s.ps[1].ps1 == 24 && s.gpr[3] == 0x20);
  CHECK(s.pc == 0x1304);
  for (PHU32 sync : {2u, 0x200u}) {
    s.Exceptions = sync; CHECK(run(0x1300) == PH_DECLINE && reads == 2);
  }
  // The MMIO callback requests an immediate timing check with only an async bit pending.
  fault = false; force_check = true; s.Exceptions = 4; s.downcount = 20;
  CHECK(run(0x1300) == PH_DISPATCH && s.pc == 0x1304 && s.downcount == -1);
  CHECK(s.gpr[3] == 0x24 && reads == 3);
  force_check = false; s.Exceptions = 0;
  static unsigned char page[1u << 17]{}; page[0x28] = 0x40; page[0x2c] = 0xbf; page[0x2d] = 0x80;
  const void* pages[] = {page}; PHU32 dbat[] = {3};
  h.logical_pages = pages; h.dbat = dbat; s.msr |= 16; s.gpr[3] = 0x24;
  run(0x1300);
  CHECK(reads == 3 && s.ps[1].ps0 == std::bit_cast<PHU64>(2.0));
  CHECK(s.ps[1].ps1 == std::bit_cast<PHU64>(-1.0) && s.pc == s.spr[8]);

  run(0x1400);
  CHECK(s.gpr[3] == 5 && fallback_pc == 0x1404 && s.pc == 0x2400);
  s.msr |= 1u << 14; h.checked_fp = 1; s.downcount = 10; fault_pc = 0;
  CHECK(run(0x1400) == PH_DISPATCH && s.pc == 0x1408 && s.Exceptions == 0x80 && !fault_pc);
  s.msr &= ~(1u << 14); s.Exceptions = 0; h.checked_fp = 0;
  s.gpr[3] = 0; s.spr[9] = 3; s.downcount = 3; run(0x1500);
  CHECK(s.gpr[3] == 2 && s.spr[9] == 1 && s.pc == 0x1500 && s.downcount == -1);
  s.xer_ca = 1; s.gpr[3] = 17; run(0x1600);
  CHECK(s.gpr[3] == 17 && !s.xer_ca);
  s.downcount = 20; run(0x1700);
  CHECK(s.spr[8] == 0x1704 && s.pc == 0x1800 && s.downcount == 19);
  s.spr[8] = 0x2340; run(0x1704);
  CHECK(s.gpr[7] == 25 && s.pc == 0x2340 && s.downcount == 17);
  for (PHU32 i = 0; i < module->entry_count; ++i)
    if (module->entries[i].entry_pc == 0x1800)
      bound[(module->entries[i].call_slot - 1) * PH_FEATURE_KEYS + s.feature_flags] = &module->entries[i];
  s.downcount = 20; s.gpr[7] = 0; run(0x1700);
  CHECK(s.gpr[6] == 17 && s.gpr[7] == 25 && s.pc == 0x1704 && s.downcount == 15);
  s.gpr[7] = 0; run(0x1700, PH_CALL_LIMIT);
  CHECK(s.pc == 0x1800 && !s.gpr[7]);
  const auto* target = bound[0];  // 0x1800 is the lowest callable address in this fixture
  CHECK(target && target->entry_pc == 0x1800);
  auto& slot = bound[(target->call_slot - 1) * PH_FEATURE_KEYS + s.feature_flags];
  const auto saved_slot = slot;
  s.feature_flags ^= 1;
  s.downcount = 20; run(0x1700); CHECK(s.pc == 0x1800 && !s.gpr[7] && s.downcount == 19);
  s.feature_flags ^= 1;
  PHAotEntry wrong = *saved_slot;
  wrong.fn = module->entries[0].fn;
  slot = &wrong;
  run(0x1700); CHECK(s.pc == 0x1800 && !s.gpr[7] && s.downcount == 18);
  slot = saved_slot;
  s.downcount = 3; run(0x1700);
  CHECK(s.pc == 0x1704 && !s.gpr[7] && s.downcount == 0);

  s.downcount = 2; s.gpr[5] = 0; run(0x1900);
  CHECK(s.pc == s.spr[8] && s.downcount == -2 && s.gpr[5] == 12);
  s.gpr[3] = 0x20; s.Exceptions = 0x80; CHECK(run(0x1d00) == PH_DECLINE);
  CHECK(s.gpr[3] == 0x20 && s.pc == 0x1d00);
  s.Exceptions = 0; s.gpr[3] = 0x80000000; s.gpr[4] = ~0u; run(0x1c00);
  CHECK(s.gpr[3] == ~0u && s.xer_so_ov == 3);
  s.gpr[3] = 2; s.downcount = 10; run(0x1e00);
  CHECK(s.gpr[3] == 9 && s.pc == s.spr[8]);
  invalidate = true; run(0x1e00);
  CHECK(s.gpr[3] == 15 && s.pc == 0x1e04);
  invalidate = false;

  // A four-byte aligned double stays on the BAT page. Then move the BAT to the FIFO.
  s.feature_flags |= PH_PERFMON; s.downcount = 20; s.gpr[3] = 4; dbat[0] |= 4;
  s.ps[1].ps0 = 0x1122334455667788ull; run(0x1a00);
  CHECK(page[4] == 0x11 && page[11] == 0x88 && pm_cycles == 2 && pm_ls == 1 && pm_fp == 1);
  unsigned char fifo[64]{};
  dbat[0] = 0x0c000001; pages[0] = nullptr; s.gpr[3] = 0x8000;
  s.gather_pipe_base_ptr = fifo; s.gather_pipe_ptr = fifo + 28;
  run(0x1a00);
  CHECK(bursts == 1 && s.gather_pipe_ptr == fifo + 4 && fifo[0] == 0x55 && fifo[3] == 0x88);
  expected_fifo_high = 0x3f800000;
  s.ps[1] = {std::bit_cast<PHU64>(1.0), std::bit_cast<PHU64>(-2.0)};
  s.gather_pipe_ptr = fifo + 28; run(0x2000);
  CHECK(bursts == 2 && fifo[0] == 0xc0 && fifo[1] == 0 && fifo[2] == 0 && fifo[3] == 0);
  s.spr[920] = 0; s.gather_pipe_ptr = fifo + 28; run(0x5120);
  CHECK(bursts == 3 && s.gather_pipe_ptr == fifo + 4 && fifo[0] == 0xc0);
  s.gpr[4] = expected_fifo_high = 0x12345678; s.gather_pipe_ptr = fifo + 28;
  run(0x4700); CHECK(bursts == 4 && s.gather_pipe_ptr == fifo);
  force_check = true; s.downcount = 20; s.gather_pipe_ptr = fifo + 28;
  CHECK(run(0x4700) == PH_DISPATCH && s.pc == 0x4704 && s.downcount == -1);
  CHECK(bursts == 5 && s.gather_pipe_ptr == fifo);
  force_check = false;
  s.spr[920] = 0x80000000;

  for (PHU32 i = 0; i < module->entry_count; ++i) {
    const auto& entry = module->entries[i];
    if (entry.entry_pc == 0x2200 || entry.entry_pc == 0x2400 || entry.entry_pc == 0x2600)
      bound[(entry.call_slot - 1) * PH_FEATURE_KEYS + s.feature_flags] = &entry;
  }
  s.downcount = 40; s.gpr[7] = 0; invalidate = true; run(0x2100);
  CHECK(s.pc == 0x2204 && !s.gpr[7] && s.downcount == 34);
  invalidate = false; s.gpr[3] = 0xaabc; run(0x2300);
  CHECK(s.pc == 0xaabc && !s.gpr[7]);
  run(0x2500); CHECK(s.pc == 0x2504 && !s.gpr[7]);
  s.cr[0] = 0; s.downcount = 12; run(0x2c00);
  CHECK(s.gpr[3] == 2 && s.gpr[4] == 7 && s.downcount == 8);
  s.cr[0] = 1; run(0x2c00); CHECK(s.gpr[3] == 1 && s.gpr[4] == 6 && s.downcount == 3);

  const void* physical[] = {page}; h.physical_pages = physical; h.ram_size = sizeof(page);
  dbat[0] = 5; s.gpr[3] = 4; s.ps[1].ps0 = 0x0102030405060708ull; run(0x1a00);
  CHECK(page[4] == 1 && page[11] == 8);
  s.gpr[3] = 4; run(0x2800);
  CHECK(s.gpr[30] == 0x01020304 && s.gpr[31] == 0x05060708 && page[12] == 1 && page[19] == 8);
  s.spr[1008] = 1u << 14; s.gpr[3] = 4; s.gpr[4] = 5; run(0x2a00);
  for (int i = 0; i < 32; ++i) CHECK(page[i] == 0);
  h.mftb = [](void*, PHU32) { CHECK(s.pc == 0x2b00); s.gpr[3] = 123456; };
  run(0x2b00); CHECK(s.gpr[3] == 123456);
  for (PHU32 value : {0u, 1u, 0x80000000u, 0xabcdef12u, ~0u}) {
    for (PHU32 shift = 0; shift < 128; ++shift) {
      s.gpr[3] = value; s.gpr[4] = shift;
      run(0x2700);
      const unsigned n = shift & 63;
      CHECK(s.gpr[3] == PHU32(PHS32(value) >> (n < 32 ? n : 31)));
      CHECK(s.xer_ca == (PHS32(value) < 0 && (n >= 32 || (value & ((1u << n)-1)))));
    }
  }
  pages[0] = page; dbat[0] = 3; s.gpr[3] = 1; page[0x46] = 0x40; page[0x47] = 0x28;
  s.downcount = 20; run(0x4000);
  CHECK(s.gpr[7] == 2 && s.pc == s.spr[8] && s.downcount == 9);
  s.downcount = 9; s.gpr[7] = 0; run(0x4000);
  CHECK(s.pc == 0x4028 && !s.gpr[7] && !s.downcount);
  for (unsigned op = 0; op < 8; ++op) {
    for (unsigned a = 0; a < 2; ++a) for (unsigned b = 0; b < 2; ++b) {
      s.cr[0] = 1; s.cr[2] = PHU64(a) << 62; s.cr[5] = PHU64(b) << 62;
      const unsigned expected[] = {a&b,a&~b,~(a^b),~(a&b),~(a|b),a|b,a|~b,a^b};
      run(0x3000 + 16*op);
      CHECK(((s.gpr[5] >> 30) & 1) == (expected[op] & 1));
    }
  }
  s.gpr[3] = 0xb0000000; run(0x2e00); CHECK((s.gpr[5] >> 28) == 11);
  s.gpr[3] = ~0u; s.gpr[4] = 0; run(0x3100);
  CHECK(!s.gpr[3] && (s.xer_so_ov & 3) == 3);
  s.gpr[3] = ~0u; s.gpr[4] = 2; run(0x3100);
  CHECK(s.gpr[3] == 0x7fffffffu && s.xer_so_ov == 2);
  s.gpr[3] = 0x80000000; run(0x3200); CHECK(s.gpr[3] == 0x80000000 && s.xer_so_ov == 3);
  // Validate both conditional LR outcomes through an actual verified native caller.
  for (PHU32 i = 0; i < module->entry_count; ++i) {
    const auto& e = module->entries[i];
    if (e.entry_pc == 0x4400 || e.entry_pc == 0x4600)
      bound[(e.call_slot-1) * PH_FEATURE_KEYS + s.feature_flags] = &e;
  }
  for (unsigned eq : {0u, 1u}) {
    s.Exceptions = PH_ASYNC_EXCEPTIONS; s.gpr[6] = s.gpr[7] = 0;
    s.cr[0] = eq ? 0 : 1; s.downcount = 20;
    CHECK(run(0x4300) == PH_RETURN);
    CHECK(s.gpr[7] == 25 && s.gpr[6] == (eq ? 0u : 11u));
    CHECK(s.pc == 0x4304 && s.downcount == (eq ? 16 : 14));
  }
  s.downcount = 30; s.gpr[5] = s.gpr[6] = s.gpr[7] = s.gpr[8] = 0;
  CHECK(run(0x4500) == PH_RETURN && s.pc == 0x450c);
  CHECK(s.gpr[4] == 4 && s.gpr[5] == 9 && s.gpr[6] == 5 && s.gpr[8] == 20);
  s.gpr[7] = 3; run(0x4204); CHECK(s.gpr[7] == 5);
  for (unsigned i = 0; i < 8; ++i) page[0x4160+i] = 0xff;
  page[0x4163] = 0xd0; page[0x4167] = 0xd8;
  for (unsigned index : {0u, 1u, 2u}) {
    s.gpr[1] = 0; s.gpr[3] = index; s.downcount = 25;
    CHECK(run(0x4100) == PH_RETURN && s.gpr[7] == 5 + index);
  }
  check_psq();
  s.Exceptions = 0;
  s.msr |= 0x900; s.fpscr = 0x80; h.checked_fp = 1; s.downcount = 20;
  s.ps[2].ps0 = 0x7ff0000000000001ull; s.ps[3].ps0 = 0; s.ps[1].ps0 = 123;
  run(0x1210); CHECK(fault_pc == 0x1210 && s.pc == 0x1210 && s.downcount == 19);
  CHECK(s.ps[1].ps0 == 123 && (s.Exceptions & 0x80) && s.spr[27] == (1u << 20));
  std::puts("synthetic native ABI, integer, FP/PS, memory, fallback and timing checks passed");
}

// Separate guest BAT pages with a canary gap so an undersized check cannot succeed
// by accidentally reading the right bytes from an adjacent host allocation.
namespace {
constexpr unsigned page_size = 1u << 17;
struct GuardedPage { unsigned char data[page_size]; unsigned char guard[8]; };
GuardedPage banks[2];
unsigned char& guest_byte(unsigned ea) { return banks[ea / page_size].data[ea % page_size]; }
unsigned psq_slow_calls;
}
void check_psq()
{
  auto saved_host = h;
  const void* pages[] = {banks[0].data, banks[1].data};
  PHU32 dbat[] = {3, 0x20003};
  h.logical_pages = pages; h.dbat = dbat; h.physical_pages = nullptr;
  h.fallback = [](void*, PHU32 word) {
    // The callback marks a delegated transaction. No host memory access is allowed
    // on this path, even when the adjacent guest page would have been valid.
    ++psq_slow_calls;
    CHECK((word >> 26) == 4 || (word >> 26) == 56 || (word >> 26) == 60);
    CHECK(s.pc >= 0x5000 && s.pc < 0x5200 && s.npc == s.pc + 4);
  };
  s.spr[920] |= 0x80000000;
  s.msr |= 0x2010;
  for (unsigned type : {4u, 5u, 6u, 7u})
    for (unsigned single : {0u, 1u})
      for (unsigned indexed : {0u, 1u})
        for (bool wi : {false, true})
          for (unsigned ea : {8u, 9u, page_size-4, page_size-2, page_size-1, page_size})
          {
            s.spr[912] = type | (type << 16);
            s.gpr[3] = ea;
            dbat[0] = dbat[1] = wi ? 5 : 3;
            const unsigned width = type == 5 || type == 7 ? 2 : 1;
            const unsigned bytes = width * (single ? 1 : 2);
            const bool slow_load = ea % width || ea % page_size + bytes > page_size;
            const bool slow_store = slow_load || (wi && (bytes < 4 || (ea & 3)));
            const unsigned load_pc = 0x5000 + 0x100*indexed + 8*single;
            const int lane[] = {type == 6 ? -97 : type == 7 ? -23456 : 189,
                                type == 5 ? 0xabcd : 23};
            unsigned char expected[4]{};
            for (unsigned i = 0; i < bytes; ++i)
              expected[i] = static_cast<unsigned char>(unsigned(lane[i/width]) >>
                                                        (8 * (width-1-i%width)));
            for (auto& bank : banks)
              for (auto& b : bank.guard) b = 0x5a;
            for (unsigned i = ea-1; i <= ea+bytes; ++i) guest_byte(i) = 0xcc;
            s.ps[1] = {std::bit_cast<PHU64>(double(lane[0])), std::bit_cast<PHU64>(double(lane[1]))};
            s.downcount = 20; psq_slow_calls = 0;
            CHECK(run(load_pc + 0x20) == PH_RETURN);
            CHECK(psq_slow_calls == unsigned(slow_store));
            for (unsigned i = 0; i < bytes; ++i) {
              CHECK(guest_byte(ea+i) == (slow_store ? 0xcc : expected[i]));
              guest_byte(ea+i) = expected[i];
            }
            CHECK(guest_byte(ea-1) == 0xcc && guest_byte(ea+bytes) == 0xcc);
            for (auto& bank : banks)
              for (auto b : bank.guard) CHECK(b == 0x5a);
            s.ps[1] = {11,22}; psq_slow_calls = 0;
            CHECK(run(load_pc) == PH_RETURN);
            CHECK(psq_slow_calls == unsigned(slow_load));
            CHECK(s.ps[1].ps0 == (slow_load ? 11 : std::bit_cast<PHU64>(double(lane[0]))));
            CHECK(s.ps[1].ps1 == (slow_load ? 22 : std::bit_cast<PHU64>(single ? 1.0 : double(lane[1]))));
          }
  // An aligned U16 pair straddles the real RAM extent inside a mapped WI page.
  pages[0] = nullptr;
  const void* physical[] = {banks[0].data};
  h.physical_pages = physical; h.ram_size = 10;
  s.spr[912] = 5u | (5u << 16); s.gpr[3] = 8; psq_slow_calls = 0;
  s.downcount = 20;
  run(0x5000); run(0x5020); CHECK(psq_slow_calls == 2);
  run(0x5008); CHECK(psq_slow_calls == 2);
  pages[0] = banks[0].data;
  h.dbat = nullptr; s.gpr[3] = 0; psq_slow_calls = 0;
  run(0x5000); CHECK(psq_slow_calls == 1);
  h = saved_host;
}
"""


# PrimeHack for iPad: adapt freestanding Clang builtins only for the MSVC test build.
MSVC_BUILTINS = r"""
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <intrin.h>
#include <limits>
#define __builtin_bit_cast(T,x) std::bit_cast<T>(x)
#define __builtin_bswap64 _byteswap_uint64
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap16 _byteswap_ushort
#define __FLT_MAX__ std::numeric_limits<float>::max()
#define __builtin_nan(x) std::numeric_limits<double>::quiet_NaN()
#define __builtin_huge_val() std::numeric_limits<double>::infinity()
#define __builtin_unreachable() __assume(0)
#define __builtin_rotateleft32(x,n) std::rotl(unsigned(x),int(n))
#define __builtin_clz(x) std::countl_zero(unsigned(x))
#define __builtin_clzll(x) std::countl_zero((unsigned long long)(x))
#define __builtin_fma std::fma
#define __builtin_trunc std::trunc
#define __builtin_ceil std::ceil
#define __builtin_floor std::floor
#define __builtin_fabs std::fabs
#define __builtin_copysign std::copysign
#define __builtin_isinf std::isinf
#define __builtin_isnan std::isnan
#define __builtin_signbit std::signbit
#define __builtin_memcpy std::memcpy
#define __builtin_memset std::memset
"""


class NativeModule(unittest.TestCase):
    @unittest.skipUnless(shutil.which("clang++") or shutil.which("cl"), "requires clang++ or MSVC x64")
    def test_span_has_idle_loop(self):
        tools = Path(__file__).resolve().parent
        core = tools.parents[1]/"Source/Core"
        source = (core/"Core/PowerPC/PPCAnalyst.cpp").read_text(encoding="utf-8")
        # Compile the production block analyzer with fixture memory, omitting the symbol database
        # discovery routines and their emulator dependencies.
        analyzer = (source[source.index("namespace PPCAnalyst"):source.index("bool AnalyzeFunction(")] +
                    source[source.index("static bool IsMfspr("):source.index("static void FindFunctionsFromBranches(")] +
                    source[source.index("static bool isCarryOp("):])
        with tempfile.TemporaryDirectory(dir=tools) as tmp:
            out = Path(tmp)
            (out/"block_analyzer.inc").write_text(analyzer, encoding="utf-8")
            # BitField.h declares formatters. This standalone executable never formats a BitField.
            (out/"fmt").mkdir()
            (out/"fmt/format.h").write_text(
                "#pragma once\nnamespace fmt { template<class> struct formatter; class format_parse_context; }\n")
            exe = out/("idle.exe" if os.name == "nt" else "idle")
            fixture = tools/"idle_loop_smoke.cpp"
            if compiler := shutil.which("clang++"):
                command = [compiler, "-std=c++20", "-I", out, "-I", core, fixture, "-o", exe]
            else:
                command = ["cl", "/nologo", "/std:c++20", "/EHsc", "/I" + str(out),
                           "/I" + str(core), fixture, "/Fe:" + str(exe)]
            env = {**os.environ, "TMP": tmp, "TEMP": tmp, "TMPDIR": tmp}
            subprocess.run(command, check=True, cwd=out, env=env)
            subprocess.run([exe], check=True, cwd=out, env=env)

    @unittest.skipUnless(shutil.which("clang++") or shutil.which("cl"), "requires clang++ or MSVC x64")
    def test_execute(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).parent) as tmp:
            out = Path(tmp).resolve()
            native_fixture(out)
            compiler = shutil.which("clang++")
            exe = out/("smoke.exe" if os.name == "nt" else "smoke")
            headers = Path(__file__).resolve().parents[2]/"Source/Core/Core/PowerPC/Aot"
            files = [out/n for n in (out/"aot_sources.txt").read_text().splitlines()]
            env = {**os.environ, "TMP": str(out), "TEMP": str(out), "TMPDIR": str(out)}
            if not compiler:
                shim = out/"msvc_builtins.h"
                shim.write_text(MSVC_BUILTINS)
                flags = [shutil.which("cl"), "/nologo", "/std:c++20", "/EHsc", "/O2", "/fp:strict",
                         "/I" + str(headers)]
                subprocess.run([*flags, "/FI" + str(shim), "/LD", *files, "/Fe:synthetic.dll"],
                               check=True, cwd=out, env=env)
                subprocess.run([*flags, "smoke.cpp", "synthetic.lib", "/Fe:" + str(exe)],
                               check=True, cwd=out, env=env)
                subprocess.run([exe], check=True, cwd=out, env=env)
                return
            subprocess.run([compiler, "-std=c++20", "-O2", "-fno-strict-aliasing",
                            "-frounding-math", "-ffp-contract=off", "-I", str(headers),
                            *files, out/"smoke.cpp", "-o", exe], check=True, cwd=out, env=env)
            subprocess.run([exe], check=True, cwd=out, env=env)


if __name__ == "__main__":
    unittest.main()
