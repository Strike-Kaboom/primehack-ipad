// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PrimeHack for iPad: compare guest-visible results of short Gekko programs.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Common/GekkoDisassembler.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Sram.h"
#include "Core/PowerPC/CachedInterpreter/CachedInterpreter.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/ExceptionUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCTables.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/CommandProcessor.h"

// PrimeHack for iPad: gtest last. Its TEST macro collides with x64Emitter::TEST, which
// CachedInterpreter.h pulls in through JitBase.h.
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

namespace
{
constexpr u32 CODE = 0x80004000;
constexpr u32 STOP = 0x80005000;
constexpr u32 MEM1_DATA = 0x10000;
constexpr u32 MEM2_DATA = 0x10010000;
constexpr u32 MAX_BODY = 24;
constexpr u32 STEP_LIMIT = 128;
constexpr u32 PROGRAM_VECTOR = 0x700;
constexpr u32 FPU_VECTOR = 0x800;
constexpr u32 SC_VECTOR = 0xc00;
constexpr u32 FIFO_RAM = 0x30000;
constexpr u32 FIFO_CAPACITY = 256;
constexpr u32 PAGE_TABLE = 0x100000;
constexpr u32 PAGED_DATA = 0x30001000;
constexpr u32 PMC_REGS[] = {SPR_PMC1, SPR_PMC2, SPR_PMC3, SPR_PMC4};
using Scratch = std::array<u8, 1024>;

struct State
{
  std::array<u32, 32> gpr{};
  std::array<std::array<u64, 2>, 32> ps{};
  std::array<u32, 8> gqr{};
  std::array<u32, 4> pmc{};
  u32 cr{}, xer{}, fpscr{}, lr{}, ctr{}, msr{}, pc{}, npc{}, srr0{}, srr1{}, exceptions{};
  u32 dar{}, dsisr{}, cycles{}, fifo_committed{};
  std::vector<u8> fifo;
  std::vector<u32> fault_cycles;
  bool reserve{};
  u32 reserve_address{};
  Scratch data{};
  bool operator==(const State&) const = default;
};

struct Program
{
  struct Segment
  {
    u32 pc, physical;
    std::vector<u32> words;
  };
  State initial;
  std::vector<u32> words;
  std::vector<Segment> extra;
  u32 physical_data;
  bool expects_trap = false;
  std::string label;
  std::string_view group;
  std::string_view known_divergence;
  std::string_view expected_record;
  u32 stop = STOP;
  u32 required_vector = 0;
  u32 fault_pc = 0;
  bool monitor = false;
  bool lsqe = true;
  bool lazy_fpu = false;
  int fifo_prefix = -1;
  u32 fifo_store_bytes = 0;
  std::vector<u8> expected_store;
  bool remap = false;
  u32 step_limit = STEP_LIMIT;
  std::string expected_alert;
  bool lifecycle = false;
  bool breakpoint = false;
  bool paged_code = false;
  bool short_slice = false;
  u32 invalidate_caller_at = 0;
};

enum class Mode
{
  Normal,
  NoPIC,
  Optimized,
  Checked
};

// Use raw mt19937 output and integer bit patterns throughout generation. The previous guest's
// rounding mode (and the host library's implementation of random distributions) cannot affect it.
class Random
{
public:
  explicit Random(u32 seed) : m_engine(seed) {}
  u32 Word() { return m_engine(); }
  u32 Below(u32 limit) { return Word() % limit; }
  u32 Pick(std::initializer_list<u32> values)
  {
    return values.begin()[Below(static_cast<u32>(values.size()))];
  }

  u64 Lane()
  {
    static constexpr u64 edges[] = {
        0, 1, 0x000fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff,
        0x7ff0000000000000, 0x7ff8000000000001, 0x7ff0000000000001,
        0x36a0000000000000, 0x3810000000000000, 0x47efffffe0000000, // binary32 limits
        0x7ff8000020000000, 0x7ff0000020000000, // payloads visible at binary32 precision
        0x3ff0000000000000, 0x4000000000000000, 0x3ff0000010000000,
    };
    const u64 hi = Word();
    const u64 bits = hi << 32 | Word();
    switch (Below(4))
    {
    case 0:
      return bits;
    case 1:
      // Finite values of both signs near unity frequently satisfy the NEON input/result guards.
      return (bits & 0x800fffffffffffffULL) | 0x3ff0000000000000ULL;
    default:
      return (bits & (1ULL << 63)) | edges[Below(std::size(edges))];
    }
  }

private:
  std::mt19937 m_engine;
};

constexpr u32 D(u32 op, u32 d, u32 a, u32 imm)
{
  return op << 26 | d << 21 | a << 16 | (imm & 0xffff);
}

constexpr u32 X(u32 op, u32 xo, u32 d, u32 a, u32 b, u32 rc = 0)
{
  return op << 26 | d << 21 | a << 16 | b << 11 | xo << 1 | rc;
}

constexpr u32 Spr(u32 xo, u32 reg, u32 spr)
{
  return X(31, xo, reg, spr & 31, spr >> 5);
}

// r1/r2 address the same scratch bytes through the cached/uncached BATs. r3 is an aligned
// index and r4 supplies mtspr XER with a bounded string length. All register ranges written by
// lmw/lsw stay above these registers. Each update moves a base by at most eight bytes.
u32 MemoryOp(Random& r)
{
  const u32 reg = 8 + r.Below(24);
  const u32 fp = r.Below(32);
  const u32 base = 1 + r.Below(2);
  const u32 disp = r.Pick({0, 8, 0xfff8});
  switch (r.Below(7))
  {
  case 0: // lwz/lbz/stw/stb/lhz/lha/sth, with update forms, lmw/stmw
  {
    const u32 op = 32 + r.Below(16);
    // Vary byte and halfword lanes, especially for subword writes through the W/I mirror.
    // Update forms keep the base doubleword-aligned for later FP and multiple-word transfers.
    if (op == 34 || op == 38)
      return D(op, reg, base, r.Below(16));
    if (op == 40 || op == 42 || op == 44)
      return D(op, reg, base, 2 * r.Below(8));
    return D(op, reg, base, disp);
  }
  case 1: // Integer indexed, update-indexed and byte-reversed forms.
    return X(31, r.Pick({23, 55, 87, 119, 151, 183, 215, 247, 279, 311, 343, 375, 407, 439,
                         534, 790, 662, 918}), reg, base, 3);
  case 2:
    return D(48 + r.Below(8), fp, base, disp);
  case 3:
    return X(31, r.Pick({535, 567, 599, 631, 663, 695, 727, 759, 983}), fp, base, 3);
  case 4:
  {
    const u32 op = r.Pick({56, 57, 60, 61});
    // fD may have become NaN anywhere in the program. Use the float GQR for random
    // stores. The directed integer stores below own their finite source register.
    const u32 w_i = r.Below(16) & (op >= 60 ? 8 : 15);
    return D(op, fp, base, w_i << 12 | (disp & 0xfff));
  }
  case 5:
  {
    const u32 xo = r.Pick({6, 7, 38, 39});
    const u32 wx_ix = r.Below(16) & (xo & 1 ? 8 : 15);
    return X(4, xo, fp, base, 3) | wx_ix << 7;
  }
  default:
  {
    const u32 xo = r.Pick({20, 150, 533, 597, 661, 725});
    const u32 rb = (xo == 597 || xo == 725) ? r.Below(32) : 3;
    return X(31, xo, 8 + reg % 16, base, rb, xo == 150);
  }
  }
}

u32 IntegerOp(Random& r)
{
  const u32 d = 8 + r.Below(24);
  const u32 a = 8 + r.Below(24);
  const u32 b = 8 + r.Below(24);
  const u32 rc = r.Below(2);
  const u32 imm = r.Word() & 0xffff;
  switch (r.Below(9))
  {
  case 0:
  case 1:
  {
    // XO form. RB is reserved in the unary instructions. OE is reserved in mulhw[u].
    const u32 xo = r.Pick({266, 10, 138, 202, 234, 40, 8, 136, 200, 232, 104, 235, 75, 11,
                           491, 459});
    const bool unary = xo == 202 || xo == 234 || xo == 200 || xo == 232 || xo == 104;
    const u32 oe = (xo != 75 && xo != 11) ? r.Below(2) : 0;
    return X(31, xo | oe << 9, d, a, unary ? 0 : b, rc);
  }
  case 2:
    return D(r.Pick({7, 8, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29}), d, a, imm);
  case 3:
  {
    const u32 xo = r.Pick({28, 60, 444, 124, 316, 412, 476, 284, 536, 792, 824, 24,
                           26, 922, 954});
    const bool unary = xo == 26 || xo == 922 || xo == 954;
    return X(31, xo, d, a, unary ? 0 : (xo == 824 ? imm & 31 : b), rc);
  }
  case 4:
  {
    const u32 op = r.Pick({20, 21, 23});
    return X(op, imm & 1023, d, a, op == 23 ? b : (imm >> 10) & 31, rc);
  }
  case 5:
  {
    const u32 bf = r.Below(8) << 2;
    if (r.Below(2))
      return X(31, r.Pick({0, 32}), bf, a, b);
    return D(r.Pick({10, 11}), bf, a, imm);
  }
  case 6:
  {
    const u32 xo = r.Pick({257, 129, 289, 225, 33, 449, 417, 193});
    return X(19, xo, imm & 31, (imm >> 5) & 31, (imm >> 10) & 31);
  }
  case 7:
    if (r.Below(2))
      return X(31, 19, d, 0, 0); // mfcr
    return X(31, 144, d, 0, 0) | (imm & 255) << 12; // mtcrf
  default:
  {
    const u32 spr = r.Pick({SPR_LR, SPR_CTR, SPR_XER});
    const bool write = r.Below(2) != 0;
    return Spr(write ? 467 : 339, write && spr == SPR_XER ? 4 : d, spr);
  }
  }
}

u32 FloatOp(Random& r, bool enables)
{
  const u32 d = r.Below(32);
  const u32 a = r.Below(32);
  const u32 b = r.Below(32);
  const u32 c = r.Below(32);
  const u32 rc = r.Below(2);
  switch (r.Below(8))
  {
  case 0:
  case 1:
  case 2:
  {
    const u32 op = r.Pick({59, 63, 4});
    u32 xo;
    if (op == 4)
      xo = r.Pick({10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31});
    else if (op == 59)
      xo = r.Pick({18, 20, 21, 24, 25, 28, 29, 30, 31});
    else
      xo = r.Pick({18, 20, 21, 23, 25, 26, 28, 29, 30, 31});
    const bool estimate = xo == 24 || xo == 26;
    const bool multiply = xo == 12 || xo == 13 || xo == 25;
    const bool uses_c = !estimate && xo != 18 && xo != 20 && xo != 21;
    return X(op, xo, d, estimate ? 0 : a, multiply ? 0 : b, rc) | (uses_c ? c << 6 : 0);
  }
  case 3:
    return X(63, r.Pick({264, 14, 15, 72, 136, 40, 12}), d, 0, b, rc);
  case 4:
  {
    const u32 xo = r.Pick({40, 136, 264, 72, 528, 560, 592, 624});
    return X(4, xo, d, xo >= 528 ? a : 0, b, rc);
  }
  case 5:
    if (r.Below(2))
      return X(4, r.Pick({0, 32, 64, 96}), d & 28, a, b);
    return X(63, r.Pick({0, 32}), d & 28, a, b);
  default:
  {
    // Keep VE/OE/UE/ZE/XE clear throughout ordinary programs, including guest FPSCR writes.
    const u32 xo = r.Pick({64, 583, 38, 70, 134, 711});
    switch (xo)
    {
    case 64: // mcrfs
      return X(63, xo, d & 28, a & 28, 0);
    case 583: // mffs
      return X(63, xo, d, 0, 0, rc);
    case 38: // mtfsb1
    case 70: // mtfsb0
    {
      // Avoid the FEX/VX summaries and the reserved FPSCR bit.
      const bool writable = d != 1 && d != 2 && d != 20;
      const bool enable_bit = d >= 24 && d <= 28;
      return X(63, xo, writable && (enables || !enable_bit) ? d : 31, 0, 0, rc);
    }
    case 134: // mtfsfi
    {
      const u32 field = enables ? d % 8 : d % 6;
      return X(63, xo, field << 2, 0, 0, rc) | (c & 15) << 12;
    }
    default: // mtfsf
      return X(63, xo, 0, 0, b, rc) | (r.Word() & (enables ? 0xff : 0xfc)) << 17;
    }
  }
  }
}

Program Generate(u32 seed, bool wii, bool enables)
{
  Random r(seed);
  Program p{};
  p.physical_data = wii ? MEM2_DATA : MEM1_DATA;
  State& s = p.initial;
  for (auto& reg : s.gpr)
  {
    reg = r.Word();
    if (r.Below(4) == 0)
      reg = r.Pick({0, 1, 31, 32, 0x7fffffff, 0x80000000, 0xffffffff});
  }
  for (auto& pair : s.ps)
    pair = {r.Lane(), r.Lane()};
  for (auto& byte : s.data)
    byte = static_cast<u8>(r.Word());
  s.gpr[1] = 0x80000000 + p.physical_data + s.data.size() / 2;
  s.gpr[2] = s.gpr[1] + 0x40000000;
  s.gpr[3] = r.Pick({8, 0xfffffff8});
  s.gpr[4] &= 0xe000ff1f;
  s.cr = r.Word();
  s.xer = r.Word() & 0xe000ff1f;
  s.lr = r.Word();
  s.ctr = r.Word();
  s.srr0 = r.Word();
  s.srr1 = r.Word();
  s.pc = CODE;
  s.msr = 0x2032; // Boot_BS2Emu's FP, IR, DR and RI
  s.fpscr = (r.Word() & 0x0007f000) | (seed & 7); // FPRF, FI, FR, NI and RN
  if (enables)
  {
    s.msr |= 0x900; // FE0 + FE1, precise exception mode
    const u32 mask = r.Word() & FPSCR_ANY_E;
    s.fpscr |= mask ? mask : FPSCR_VE;
  }
  else
  {
    s.fpscr |= r.Word() & (FPSCR_FX | FPSCR_ANY_X);
    if (s.fpscr & FPSCR_VX_ANY)
      s.fpscr |= FPSCR_VX;
  }
  constexpr u32 types[] = {QUANTIZE_FLOAT, QUANTIZE_U8, QUANTIZE_U16, QUANTIZE_S8, QUANTIZE_S16};
  for (size_t i = 0; i < s.gqr.size(); ++i)
  {
    const u32 load = types[i % std::size(types)];
    const u32 store = types[(i + seed % std::size(types)) % std::size(types)];
    s.gqr[i] = (r.Word() & 0x3f003f00) | load << 16 | store;
  }
  s.gqr[0] = 0;
  s.reserve = r.Below(4) == 0;
  s.reserve_address = s.gpr[1] + s.gpr[3] + 4 * r.Below(2);
  s.dar = r.Word();
  s.dsisr = r.Word();

  const u32 length = 1 + r.Below(MAX_BODY);
  if (enables && seed % 16 == 0)
  {
    s.ps[1][0] = 0x3ff0000000000000;
    s.ps[2][0] = 0;
    s.fpscr |= FPSCR_ZE;
    p.words.push_back(X(63, 18, 0, 1, 2)); // fdiv f0,f1,f2 must take the program vector
    p.expects_trap = true;
  }
  else if (length >= 2)
  {
    // Plant a few patterns whose operand dependencies are unlikely in independent random words.
    switch (r.Below(8))
    {
    case 0:
      s.ctr = 2 + r.Below(3);
      p.words = {D(14, 8, 8, 1), D(16, 16, 0, 0xfffc) | r.Below(2)}; // addi; bdnz[l]
      break;
    case 1:
      p.words = {D(15, 8, 0, r.Word()), D(24, 8, 8, r.Word())}; // lis; ori
      break;
    case 2:
      p.words = {D(36, 8, 1, 0), D(32, 9, 2, 0)}; // stw; lwz through the other BAT
      break;
    case 3:
      p.words = {X(31, 20, 8, 1, 3), X(31, 150, 9, 1, 3, 1)}; // lwarx; stwcx.
      break;
    case 4:
      s.ps[1] = {0x3ff0000000000000, 0x4000000000000000};
      p.words = {X(4, 21, 0, 1, 1), X(4, 20, 0, 0, 1, 1)}; // ps_add; ps_sub.
      break;
    case 5:
      if (length >= 3)
        p.words = {0x48000009, 0x48000008, 0x4e800020}; // bl .+8; b .+8; blr
      break;
    case 6:
      if (length >= 3)
      {
        const u32 bi = r.Below(32);
        // Jump over crclr. A taken backward branch executes crclr, then falls through.
        p.words = {0x48000008, X(19, 193, bi, bi, bi), D(16, 12, bi, 0xfffc) | r.Below(2)};
      }
      break;
    }
  }
  while (p.words.size() < length)
  {
    switch (r.Below(10))
    {
    case 0:
      if (p.words.size() + 2 <= length)
      {
        const u32 bo = r.Pick({4, 12}); // branch on CR bit clear/set, optionally set LR
        const u32 bi = r.Below(32);
        p.words.push_back(D(16, bo, bi, 8) | r.Below(2));
        break;
      }
      [[fallthrough]];
    case 1:
    case 2:
    case 3:
      p.words.push_back(MemoryOp(r));
      break;
    case 4:
    case 5:
    case 6:
      p.words.push_back(IntegerOp(r));
      break;
    default:
      p.words.push_back(FloatOp(r, enables));
      break;
    }
  }
  const u32 branch_pc = CODE + 4 * length;
  p.words.push_back(18U << 26 | (STOP - branch_pc));
  return p;
}

constexpr u32 Branch(s32 displacement, bool link = false)
{
  return (18u << 26) | (static_cast<u32>(displacement) & 0x03fffffc) | static_cast<u32>(link);
}

// Start each directed sequence at its own block entry. A preceding instruction must not
// accidentally consume the first half of the pair under test.
class Corpus
{
public:
  explicit Corpus(bool wii) : m_base(Generate(1, wii, false))
  {
    m_base.words.clear();
    m_base.initial.fpscr = 0;
    m_base.initial.reserve = false;
    m_base.initial.ps[1] = {0x3ff0000000000000, 0xc000000000000000};
    m_base.initial.ps[2] = {0x4000000000000000, 0x4008000000000000};
  }

  Program& Add(std::string label, std::vector<u32> body)
  {
    auto& p = cases.emplace_back(m_base);
    p.label = std::move(label);
    p.group = group;
    p.words = std::move(body);
    return p;
  }

  std::vector<Program> cases;
  std::string_view group = "NEON smoke";

private:
  Program m_base;
};

void IntegerCases(Corpus& c)
{
  constexpr u32 ops[] = {D(14, 8, 0, -1),     D(14, 9, 8, 7),      X(21, 31, 9, 8, 3),
                         X(31, 444, 8, 9, 8), X(31, 266, 8, 9, 8), Spr(339, 8, SPR_LR),
                         Spr(467, 8, SPR_LR), D(32, 8, 1, 0),      D(36, 8, 1, 0),
                         D(34, 8, 1, 0),      D(38, 8, 1, 0)};
  for (u32 a : ops)
    for (u32 b : ops)
      c.Add(fmt::format("integer pair {:08x}/{:08x}", a, b), {a, b});
  c.Add("integer pair emission", {D(14, 9, 8, 7), D(32, 8, 1, 0)}).expected_record = "MicroOpPair(";
  c.Add("addis/ori with overlapping low bits", {D(15, 9, 8, 0xf012), D(24, 9, 9, 0xff)})
      .initial.gpr[8] = 0xff;
  c.Add("li, lis, lis/ori, identity rotate",
        {D(14, 8, 0, 0x8000), D(15, 9, 0, 0x8000), D(15, 10, 0, 0xfedc), D(24, 10, 10, 0xba98),
         X(21, 31, 10, 10, 0)});
  for (u32 op : {24, 25, 26, 27})
    c.Add("zero and consecutive logical immediates",
          {D(op, 8, 8, 0), D(op, 9, 9, 0x1357), D(op, 9, 9, 0xf00f)});
  c.Add("mcrf, mcrxr, dead CR0", {X(19, 0, 12, 20, 0), X(31, 512, 16, 0, 0),
                                  X(31, 266, 8, 9, 10, 1), X(31, 266, 11, 8, 9, 1)});
  for (u32 xo : {54, 86, 470})
    c.Add("counted cache-line loop", {X(31, xo, 0, 0, 1), D(14, 1, 1, 32), D(16, 16, 0, -8)})
        .initial.ctr = 4;
  c.Add("counted store loop", {D(38, 8, 1, 0), D(38, 8, 1, 1), D(14, 1, 1, 2), D(16, 16, 0, -12)})
      .initial.ctr = 5;
  c.Add("dcbz, zero-RA indexed", {X(31, 1014, 0, 1, 3), X(31, 151, 8, 0, 1), X(31, 23, 9, 0, 1)});
  for (bool match : {false, true})
  {
    auto& s = c.Add("initial reservation", {X(31, 150, 8, 1, 3, 1)}).initial;
    s.reserve = true;
    s.reserve_address = s.gpr[1] + s.gpr[3] + (match ? 0 : 4);
  }
}

constexpr u32 A(u32 op, u32 xo, u32 d, u32 a, u32 b, u32 c, u32 rc = 0)
{
  return X(op, xo, d, a, b, rc) | c << 6;
}

void FPPairCases(Corpus& corpus)
{
  constexpr u32 mul = A(59, 25, 1, 1, 0, 2), add = X(59, 21, 1, 1, 2);
  constexpr u32 sub = X(59, 20, 1, 1, 2), madd = A(59, 29, 1, 1, 1, 2);
  constexpr u32 msub = A(59, 28, 1, 1, 1, 2), nmsub = A(59, 30, 1, 1, 1, 2);
  constexpr u32 nmadd = A(59, 31, 1, 1, 1, 2);
  constexpr u32 frsp = X(63, 12, 1, 0, 1), fmr = X(63, 72, 1, 0, 2);
  constexpr u32 fneg = X(63, 40, 1, 0, 2), fabs_op = X(63, 264, 1, 0, 2);
  constexpr u32 lfs = D(48, 1, 5, 0), lfd = D(50, 1, 5, 0);
  constexpr u32 stfs = D(52, 1, 5, 0), stfd = D(54, 1, 5, 0);
  constexpr u32 psq_l = D(56, 1, 5, 0), psq_st = D(60, 1, 5, 0);
  constexpr u32 ps_muls0 = A(4, 12, 1, 1, 0, 2), ps_muls1 = A(4, 13, 1, 1, 0, 2);
  constexpr u32 ps_madds0 = A(4, 14, 1, 1, 1, 2), ps_madds1 = A(4, 15, 1, 1, 1, 2);
  constexpr u32 ps_sub = X(4, 20, 1, 1, 2), ps_add = X(4, 21, 1, 1, 2);
  constexpr u32 ps_mul = A(4, 25, 1, 1, 0, 2), ps_madd = A(4, 29, 1, 1, 1, 2);
  constexpr u32 ps_nmsub = A(4, 30, 1, 1, 1, 2), ps_merge00 = X(4, 528, 1, 1, 2);
  constexpr u32 ps_sum0 = A(4, 10, 1, 1, 2, 3), ps_sum1 = A(4, 11, 1, 1, 2, 3);
  constexpr u32 pairs[][2] = {
      {lfs, mul},          {lfs, add},          {lfs, sub},        {lfs, madd},
      {lfs, msub},         {lfs, nmsub},        {lfs, nmadd},      {lfs, fneg},
      {lfs, fabs_op},      {lfs, fmr},          {lfd, frsp},       {mul, stfs},
      {add, stfs},         {sub, stfs},         {madd, stfs},      {msub, stfs},
      {fneg, stfs},        {fabs_op, stfs},     {fmr, stfd},       {frsp, stfs},
      {mul, add},          {mul, mul},          {mul, madd},       {madd, madd},
      {madd, add},         {psq_l, ps_muls0},   {psq_l, ps_madds1}, {psq_l, ps_madds0},
      {psq_l, ps_mul},     {psq_l, ps_madd},    {psq_l, ps_add},   {psq_l, ps_sub},
      {psq_l, ps_merge00}, {ps_madds1, psq_st}, {ps_madds0, psq_st}, {ps_madd, psq_st},
      {ps_add, psq_st},    {ps_sub, psq_st},    {ps_mul, psq_st},  {ps_muls0, ps_madds1},
      {ps_muls0, ps_madds0}, {ps_muls0, ps_nmsub}, {ps_muls1, ps_madds1},
      {ps_madds0, ps_madds1}, {ps_madds1, ps_madds0}, {ps_madds0, ps_madds0},
      {ps_mul, ps_madd},   {ps_madd, ps_madd},  {ps_mul, ps_add},  {ps_mul, ps_mul},
      {ps_merge00, ps_muls0}, {ps_merge00, ps_mul}, {ps_add, ps_sum0}, {ps_madd, ps_sum0},
      {ps_add, ps_sum1},   {ps_madd, ps_sum1},  {lfs, lfs},        {stfs, stfs},
      {lfd, lfd},          {stfd, stfd},        {lfs, stfs},       {psq_l, psq_l},
      {psq_st, psq_st},
  };
  static_assert(std::size(pairs) == 63);
  for (const auto& [first, second] : pairs)
  {
    const auto is_mem = [](u32 inst) {
      const u32 op = inst >> 26;
      return (op >= 48 && op <= 55) || op == 56 || op == 60;
    };
    const bool mem1 = is_mem(first);
    const bool mem2 = is_mem(second);
    for (u32 cold = 0; cold < 4; ++cold)
    {
      if (((cold & 1) && !mem1) || ((cold & 2) && !mem2))
        continue;
      auto& p = corpus.Add(fmt::format("FP pair {:08x}/{:08x} cold={}", first, second, cold),
                           {first, mem2 ? second + (1 << 16) : second});
      p.initial.gpr[5] = cold & 1 ? PAGED_DATA + 512 : p.initial.gpr[1];
      p.initial.gpr[6] = cold & 2 ? PAGED_DATA + 512 : p.initial.gpr[1];
      if (!cold)
        p.expected_record = "MicroOpPair(";
    }
  }
}

void ArithmeticCases(Corpus& corpus, bool enables)
{
  constexpr u64 one = 0x3ff0000000000000, inf = 0x7ff0000000000000, sign = 1ULL << 63;
  // Columns are FA, FB, FC. Distinct payloads identify the selected operand on a NaN tail.
  constexpr u64 inputs[][3] = {
      {one, one, one},
      {inf | 0x8000000000111, sign | inf | 0x222, inf | 0x8000000000333},
      {sign | inf | 0x111, inf | 0x8000000000222, sign | inf | 0x333},
      {0, sign, inf},
      {inf, inf, 0},
      {one, 0, one},
      {1, sign | 1, one},
      {0x3810000000000000, 0, one},
      {0x380fffffe0000000, 0, one},
      {one, 0x3e70000000000000, one},
      {one + 1, 0x3e70000000000000, one},
      {one, sign | one, 0x3ffffffff8000000},
      {0x41dfffffffc00000, 0xc1e0000000200000, one},
      {0xc1e0000000000000, 0x41e0000000000000, one},
      {one, 0x41dfffffffc00000, one},
      {one, 0xc1e0000000000000, one},
      {one, sign | inf, one},
      {one, one, sign | inf | 0x555},
  };
  for (u32 op : {4, 59, 63})
  {
    const std::vector<u32> xos =
        op == 4
            ? std::vector<u32>{10, 12, 13, 14, 15, 20, 21, 25, 28, 29, 30, 31, 528, 560, 592, 624}
            : (op == 59 ? std::vector<u32>{18, 20, 21, 25, 28, 29, 30, 31}
                        : std::vector<u32>{12, 15, 18, 20, 21, 23, 25, 28, 29, 30, 31, 40, 72, 136,
                                           264});
    for (u32 xo : xos)
    {
      const bool unary = op == 63 && (xo == 12 || xo == 15 || xo >= 40);
      const bool only_ac = xo == 25 || (op == 4 && (xo == 12 || xo == 13));
      const bool has_c = !unary && xo < 32 && xo != 18 && xo != 20 && xo != 21;
      for (u32 i = 0; i < 32; ++i)
      {
        const u32 fc = i, fa = (i + 1) % 32, fb = (i + 2) % 32;
        const u32 fd = i % 4 == 0 ? fa : i % 4 == 1 ? fb : i % 4 == 2 ? fc : (i + 3) % 32;
        for (u32 rc : {0, 1})
        {
          auto& p =
              corpus.Add(fmt::format("FP op={}/{} FC={} Rc={}", op, xo, i, rc),
                         {A(op, xo, fd, unary ? 0 : fa, only_ac ? 0 : fb, has_c ? fc : 0, rc)});
          auto& s = p.initial;
          const auto& v = inputs[i % std::size(inputs)];
          // Exchange the exceptional and finite lanes, retaining unrelated PS1 on scalar ops.
          const u32 lane = op == 4 ? rc : 0;
          s.ps[fa] = {one, one};
          s.ps[fa][lane] = v[0];
          s.ps[fb] = {one | sign, one | sign};
          s.ps[fb][lane] = v[1];
          s.ps[fc] = {one + 1, one + 1};
          s.ps[fc][lane] = v[2];
          s.fpscr = (i & 7) | 0x60000 | FPSCR_FX; // NI/RN, FI, FR and sticky FX
          if (enables)
          {
            s.fpscr |= FPSCR_VE | FPSCR_ZE;
            s.msr |= ((i & 1) ? 0x800 : 0) | ((i & 2) ? 0x100 : 0);
          }
        }
      }
    }
  }
}

void BranchCases(Corpus& c, bool enables)
{
  for (u32 bf = 0; bf < 8; ++bf)
  {
    for (u32 bit = 0; bit < 4; ++bit)
    {
      const u32 bi = 4 * bf + bit;
      for (u32 bo : {4, 12})
      {
        const u32 bc = D(16, bo, bi, 8);
        for (u32 cmp : {X(31, 0, bf * 4, 8, 9), X(31, 32, bf * 4, 8, 9), D(11, bf * 4, 8, 1),
                        D(10, bf * 4, 8, 1)})
          c.Add("integer compare/BC", {cmp, bc, D(14, 10, 10, 1)});
        // Consecutive compares use the same field. Each condition selects a distinct marker.
        for (u32 xo : {0, 32})
        {
          auto& p = c.Add(fmt::format("FCmpBranch CR{} bit {} BO {} XO {}", bf, bit, bo, xo), {});
          p.initial.fpscr = 0x1f000;
          p.monitor = bf == 7;
          p.initial.ps[3][0] = 0x7ff8000022222222;
          if (enables)
          {
            p.initial.msr |= 0x900;
            p.initial.fpscr |= FPSCR_ZE; // unordered compare still reaches its branch
          }
          u32 value = 1;
          for (u32 rhs : {2, 1, 3, 2})
          {
            p.words.insert(p.words.end(),
                           {X(63, xo, bf * 4, value == 8 ? 2 : 1, value == 8 ? 1 : rhs), bc,
                            D(14, 10, 10, value)});
            value *= 2;
          }
        }
      }
    }
  }
  const u32 cmp = X(63, 0, 0, 1, 2);
  for (u32 bc :
       {D(16, 12, 0, 4), D(16, 4, 0, 0), D(16, 12, 0, 8) | 1, D(16, 8, 0, 8), D(16, 12, 4, 8)})
    c.Add("FCmpBranch fallthrough or rejected shape", {cmp, bc, D(14, 8, 8, 1)});
  c.Add("intervening record", {cmp, X(31, 19, 8, 0, 0), D(16, 12, 0, 8), D(14, 8, 8, 1)});
  c.Add("breakpoint on BC", {cmp, D(16, 12, 0, 8), D(14, 8, 8, 1)}).breakpoint = true;
  auto& absolute = c.Add("absolute BC", {cmp, D(16, 12, 0, 0x400c) | 2, D(14, 8, 8, 1)});
  absolute.initial.pc = 0x4000;
  absolute.stop = 0x5000;
  c.Add("backward FCmpBranch", {Branch(8), X(63, 72, 1, 0, 2), cmp, D(16, 12, 0, -8)});
  for (u32 xo : {0, 32})
  {
    auto& alt = c.Add("same FCmpBranch alternates taken and untaken",
                      {X(63, 40, 1, 0, 1), X(63, xo, 8, 1, 2), D(16, 12, 8, 8), D(14, 9, 9, 1),
                       D(16, 16, 0, -16)});
    alt.initial.ctr = 6;
    alt.initial.ps[2][0] = 0;
    alt.initial.fpscr = FPRF_MASK | (enables ? FPSCR_ZE : 0u);
    if (enables)
      alt.initial.msr |= 0x900;
  }
  c.Add("standalone FCmpBranch emission", {cmp, D(16, 12, 0, 8), D(14, 8, 8, 1)}).expected_record =
      "FCmpBranch(";
  c.Add("standalone CmpBranch emission", {D(11, 0, 8, 7), D(16, 12, 0, 8), D(14, 8, 8, 1)})
      .expected_record = "CmpBranch(";
  for (u32 bo : {0, 2, 8, 10, 16, 18})
  {
    auto& s = c.Add("CR/CTR loop", {D(14, 8, 8, 1), D(16, bo, 0, -4)}).initial;
    s.ctr = (bo & 2) ? 1 : 4;
    s.cr = (bo & 8) ? 0x80000000 : 0;
  }
  for (u32 xo : {16, 528})
    for (u32 link : {0, 1})
    {
      // Each iteration reloads the target even when LK replaces LR. cmpwi stops the loop.
      auto& s = c.Add("conditional bclr/bcctr loop",
                      {Spr(467, 6, xo == 16 ? SPR_LR : SPR_CTR), D(14, 8, 8, 1), D(11, 0, 8, 4),
                       X(19, xo, 12, 0, 0, link)})
                    .initial;
      s.gpr[6] = CODE | 3;
      s.gpr[8] = 0;
    }
  for (u32 link : {0, 1})
  {
    auto& p = c.Add("changing bctr targets", {Spr(467, 6, SPR_CTR), X(19, 528, 20, 0, 0, link),
                                              D(14, 6, 6, 8), Branch(-12), Spr(339, 8, SPR_LR)});
    p.initial.gpr[6] = CODE + 8 + 3;
  }
  c.Add("several callers of one blr",
        {Branch(16, true), Branch(12, true), Branch(8, true), Branch(8), X(19, 16, 20, 0, 0)});
}

void MemoryCases(Corpus& c, bool checked)
{
  for (u32 type : {QUANTIZE_FLOAT, QUANTIZE_U8, QUANTIZE_S8, QUANTIZE_U16, QUANTIZE_S16})
    for (u32 scale = 0; scale < 64; ++scale)
      for (u32 w : {0, 1})
      {
        const u32 wi = w * 8 + 3;
        auto& p =
            c.Add(fmt::format("PSQ type={} scale={} W={}", type, scale, w),
                  {D(56, 8, 1, wi << 12), D(57, 9, 1, wi << 12 | 1), X(4, 6, 10, 1, 3) | wi << 7,
                   X(4, 38, 11, 1, 3) | wi << 7, D(60, 1, 2, wi << 12), D(61, 1, 2, wi << 12 | 16),
                   X(4, 7, 1, 2, 3) | wi << 7, X(4, 39, 1, 2, 3) | wi << 7});
        p.physical_data = (p.physical_data & 0xf0000000) + 0x20000 - 512;
        auto& s = p.initial;
        // Sweep the last byte/halfword/word and an aligned interior for every type.
        const u32 offset = std::array<u32, 4>{511, 510, 508, 496}[scale % 4];
        s.gpr[1] = 0x80000000 + p.physical_data + offset;
        s.gpr[2] = s.gpr[1] + 0x40000000;
        s.gpr[3] = 4;
        s.data[offset] = 0x80;
        s.data[offset + 1] = 0;
        s.data[offset + 2] = 0x7f;
        s.data[offset + 3] = 0xff;
        s.gqr[3] = (type << 16) | type | (scale << 24) | (scale << 8);
        s.ps[1] = {0xc0e0002000000000, 0x40efffe000000000}; // both finite at every store
      }
  for (u32 offset : {1, 2, 4, 7})
  {
    auto& p = c.Add("BAT boundary and unaligned memory",
                    {D(50, 8, 1, 0), D(54, 1, 1, 0), D(48, 9, 1, 0), D(52, 2, 1, 0),
                     D(56, 10, 1, 0), D(60, 1, 1, 0), D(46, 28, 1, 0), D(47, 28, 1, 0)});
    p.physical_data = (p.physical_data & 0xf0000000) + 0x20000 - 512;
    if (offset & 3)
    {
      // PSQ and ordinary integer accesses support byte alignment. Scalar FP alignment
      // exceptions have their own directed cases below.
      p.words = {D(56, 10, 1, 0), D(60, 1, 1, 0), D(32, 8, 1, 0), D(36, 9, 1, 0)};
    }
    p.initial.gpr[1] = 0x80000000 + p.physical_data + 512 - offset;
  }
  auto& low =
      c.Add("rA=0, signed displacement extremes, wrapping EA",
            {D(50, 8, 0, 0x6000), D(54, 1, 0, 0x6008), D(48, 9, 0, 0x600c), D(52, 2, 0, 0x6010),
             D(48, 10, 5, 0x7fff), D(54, 8, 6, 0x8000), X(31, 535, 11, 0, 7), X(31, 727, 2, 0, 7)});
  low.physical_data = 0x6000;
  low.initial.gpr[0] = 0xaaaabbbb;
  low.initial.gpr[5] = 0x6020u - 0x7fffu;
  low.initial.gpr[6] = 0x6020u + 0x8000u;
  low.initial.gpr[7] = 0x6018;
  if (checked)
  {
    c.group = "alignment";
    for (u32 op : {48, 50, 52, 54})
    {
      auto& p = c.Add(fmt::format("scalar FP alignment exception op={}", op), {D(op, 8, 1, 1)});
      p.known_divergence = "Cached Interpreter leaves the alignment exception pending";
      p.required_vector = 0x600;
      // Interpreter::SingleStep delivers ALIGNMENT after advancing PC.
      p.fault_pc = CODE + 4;
    }
    c.group = "reserved GQR";
    for (u32 invalid : {1, 2, 3})
      for (u32 op : {56, 60})
      {
        auto& p = c.Add("reserved GQR encoding", {D(op, 8, 1, 0)});
        p.initial.gqr[0] = invalid << 16 | invalid;
        p.expected_alert = "PS dequantize - unknown type to read";
      }
    c.group = "checked DSI";
    for (u32 op : {48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61})
      for (bool pending : {false, true})
      {
        auto& p = c.Add(fmt::format("checked DSI op={} pending={}", op, pending), {D(op, 8, 1, 4)});
        p.required_vector = 0x300;
        p.fault_pc = CODE;
        if (pending)
        {
          p.initial.exceptions = EXCEPTION_DSI;
          if (op >= 48 && op <= 55)
            p.known_divergence = "Cached Interpreter's scalar FP fast path ignores a pending DSI";
        }
        else
          p.initial.gpr[1] = 0x30000000; // no BAT or page-table entry
      }
    c.group = "LSQE";
    for (u32 op : {56, 57, 60, 61})
    {
      auto& p = c.Add("LSQE disabled", {D(op, 8, 1, 0)});
      p.lsqe = false;
      p.required_vector = PROGRAM_VECTOR;
      p.fault_pc = CODE;
    }
    c.group = "split-page faults";
    for (u32 word :
         {D(51, 8, 5, 4), D(55, 8, 5, 4), D(57, 8, 5, 4), D(61, 8, 5, 4), X(4, 38, 8, 5, 6),
          X(4, 39, 8, 5, 6), X(31, 631, 8, 5, 6), X(31, 759, 8, 5, 6)})
    {
      auto& p = c.Add("update access with unmapped second word", {word});
      p.physical_data += 4096 - 512;
      p.initial.gpr[5] = 0x30006ff8;
      p.initial.gpr[6] = 4;
      p.required_vector = 0x300;
      p.fault_pc = CODE;
    }
  }
}

void FifoCases(Corpus& c)
{
  constexpr u32 gp = 0xcc008000;
  for (int prefix = 0; prefix < 32; ++prefix)
  {
    for (u32 word :
         {D(60, 1, 5, 0), D(60, 1, 5, 0x8000), X(4, 7, 1, 5, 6), X(4, 7, 1, 5, 6) | (1u << 10),
          D(54, 1, 5, 0), D(61, 1, 5, 0), X(4, 39, 1, 5, 6)})
    {
      // The first two-word store can cross the threshold at either word. Repeated stores
      // retain both a RAM burst and a tail, including when the initial pipe is empty.
      auto& p = c.Add(fmt::format("FIFO prefix={} store={:08x}", prefix, word),
                      {word, D(54, 2, 5, 0), D(52, 1, 5, 0), D(54, 1, 5, 0), D(54, 2, 5, 0)});
      p.fifo_prefix = prefix;
      const bool single = word == D(60, 1, 5, 0x8000) || word == (X(4, 7, 1, 5, 6) | (1u << 10));
      p.fifo_store_bytes = (single ? 4 : 8) + 28;
      p.initial.gpr[5] = gp;
      p.initial.gpr[6] = 0;
      // Snapshot immediately after this store too. Check the committed/tail split before
      // later writes could hide a missed burst, with a byte oracle independent of both cores.
      auto& first = c.Add("first FIFO store at threshold", {word});
      first.fifo_prefix = prefix;
      first.initial.gpr[5] = gp;
      first.initial.gpr[6] = 0;
      first.expected_store = word == D(54, 1, 5, 0)
                                 ? std::vector<u8>{0x3f, 0xf0, 0, 0, 0, 0, 0, 0}
                                 : std::vector<u8>{0x3f, 0x80, 0, 0, 0xc0, 0, 0, 0};
      first.fifo_store_bytes = single ? 4 : 8;
      first.expected_store.resize(first.fifo_store_bytes);
    }
  }
  auto& p = c.Add("fused lbz/stb GP copy", {});
  p.fifo_prefix = 28;
  p.fifo_store_bytes = 12;
  p.initial.gpr[5] = gp + 0x8000;
  for (u32 i = 0; i < p.fifo_store_bytes; ++i)
  {
    p.words.push_back(D(34, 8, 1, i));
    p.words.push_back(D(38, 8, 5, 0x8000));
  }
  for (u32 word : {D(60, 1, 5, 0), X(4, 7, 1, 5, 6), D(54, 1, 5, 0)})
  {
    auto& split = c.Add("split RAM / FIFO mapping", {word});
    split.physical_data += 4096 - 512;
    split.initial.gpr[5] = 0x30003ffc;
    split.initial.gpr[6] = 0;
    split.fifo_prefix = 28;
    split.fifo_store_bytes = 4;
  }
  auto& mmio = c.Add("PI FIFO MMIO loads", {D(48, 8, 5, 0), D(48, 9, 5, 4), D(32, 8, 5, 0)});
  mmio.initial.gpr[5] = 0xcc00300c; // FIFO base and end are adjacent readable u32 registers
  mmio.fifo_prefix = 0;             // install deterministic PI values on both runs
}

void FeatureCases(Corpus& c)
{
  // These three calls enter the same body with FP on, off (mtmsr), and off again (sc/rfi).
  // Re-entry after the unavailable handler retries the faulting instruction with FP on.
  constexpr u32 rfi = 0x4c000064;
  for (u32 prefix : {0u, D(14, 9, 9, 1), D(32, 9, 1, 0)})
  {
    auto& p = c.Add("lazy FPU after mtmsr and sc/rfi",
                    {Branch(28, true), X(31, 146, 5, 0, 0), Branch(20, true), 0x44000002,
                     Branch(12, true), Branch(0), Branch(0)});
    p.lazy_fpu = true;
    p.initial.gpr[5] = p.initial.msr & ~0x2000;
    p.initial.gpr[20] = p.initial.gpr[22] = 0;
    p.fault_pc = CODE + 28 + (prefix ? 4 : 0);
    if (prefix)
      p.words.push_back(prefix);
    p.words.insert(p.words.end(), {X(63, 72, 8, 0, 1), X(19, 16, 20, 0, 0)});
    p.words[5] = Branch(static_cast<s32>(4 * (p.words.size() - 5)));
    p.extra.push_back({FPU_VECTOR,
                       FPU_VECTOR,
                       {Spr(339, 21, SPR_SRR0), D(14, 20, 20, 1), Spr(339, 23, SPR_SRR1),
                        D(24, 23, 23, 0x2000), Spr(467, 23, SPR_SRR1), rfi}});
    p.extra.push_back({SC_VECTOR, SC_VECTOR, {D(14, 22, 22, 1), Spr(467, 5, SPR_SRR1), rfi}});
  }
  c.group = "entry keys and PMCs";
  for (u32 key = 0; key < 16; ++key)
  {
    auto& p = c.Add(fmt::format("entry feature key {}", key), {D(32, 8, 1, 0), X(63, 72, 8, 0, 1)});
    p.initial.pc = 0x4000;
    p.stop = 0x5000;
    p.initial.gpr[1] = (key & 1 ? 0x80000000 : 0) + p.physical_data;
    p.initial.msr = 2 | ((key & 3) << 4) | ((key & 8) ? 0x2000 : 0);
    p.monitor = key & 4;
    if (!(key & 8))
    {
      if (p.monitor)
        p.known_divergence = "Cached Interpreter omits PMC updates on the FPU-unavailable exit";
      p.required_vector = FPU_VECTOR;
      p.fault_pc = p.initial.pc + 4;
    }
  }
  c.group = "code invalidation";
  auto& inv =
      c.Add("self-invalidating icbi loop", {X(31, 982, 0, 0, 6), D(14, 8, 8, 1), D(16, 16, 0, -8)});
  inv.initial.gpr[6] = CODE;
  inv.initial.ctr = 25;
  constexpr u32 target = CODE + 0x100;
  auto& rewrite = c.Add("rewrite a warm target and execute it again",
                        {Branch(0x100, true), D(36, 5, 6, 0), X(31, 982, 0, 0, 6),
                         Branch(0x100 - 12, true), D(32, 10, 6, 0)});
  rewrite.initial.gpr[5] = D(14, 9, 9, 7);
  rewrite.initial.gpr[6] = target;
  rewrite.extra = {{target, target & 0x1fffffff, {D(14, 9, 9, 1), X(19, 16, 20, 0, 0)}}};
  auto& miss = c.Add("invalidate the caller before its indirect miss is filled",
                     {X(19, 528, 20, 0, 0, 1), X(19, 528, 20, 0, 0, 1)});
  miss.initial.ctr = target | 3;
  miss.extra = {{target, target & 0x1fffffff, {D(14, 8, 8, 1), X(19, 16, 20, 0, 0)}}};
  miss.invalidate_caller_at = target;
  // Separate BAT for an effective address that continues to exist across the physical remap.
  c.group = "BAT remap";
  constexpr u32 alias = 0x60000000;
  auto& bat = c.Add("warm target, IBAT remap, return",
                    {Spr(467, 5, SPR_IBAT3L), Spr(467, 6, SPR_IBAT3U), Spr(467, 7, SPR_CTR),
                     X(19, 528, 20, 0, 0, 1), Spr(467, 8, SPR_IBAT3L), X(19, 528, 20, 0, 0, 1)});
  bat.initial.gpr[5] = 0x40002;
  bat.initial.gpr[6] = alias | 2;
  bat.initial.gpr[7] = alias | 3;
  bat.initial.gpr[8] = 0x60002;
  bat.extra = {{alias, 0x40000, {D(14, 9, 9, 1), X(19, 16, 20, 0, 0)}},
               {alias, 0x60000, {D(14, 9, 9, 7), X(19, 16, 20, 0, 0)}}};
  bat.remap = true;
  // The PTE stays valid while the guest replaces its RPN, invalidates both TLB ways, then
  // calls the same EA again. Both old physical blocks can remain allocated.
  c.group = "PTE remap";
  constexpr u32 paged = 0x30002000;
  auto& tlb = c.Add("live instruction PTE remap",
                    {Spr(467, 7, SPR_CTR), X(19, 528, 20, 0, 0, 1), D(36, 8, 6, 0),
                     X(31, 306, 0, 0, 7), X(19, 528, 20, 0, 0, 1)});
  tlb.initial.gpr[6] = 0x80000000 + PAGE_TABLE + 2 * 64 + 4;
  tlb.initial.gpr[7] = paged;
  tlb.initial.gpr[8] = 0x60002;
  tlb.extra = {{paged, 0x40000, {D(14, 9, 9, 1), X(19, 16, 20, 0, 0)}},
               {paged, 0x60000, {D(14, 9, 9, 7), X(19, 16, 20, 0, 0)}}};
  tlb.paged_code = true;
  tlb.known_divergence = "Cached Interpreter can reuse the old instruction block after tlbie";
  c.group = "link limits";
  // Alternate two already-compiled targets at a single bctr. Its per-site prediction
  // misses on every change, while the two global entries can both stay resident.
  constexpr u32 other_target = target + 64;
  auto& polymorphic = c.Add("global hit after per-site miss",
                            {Spr(467, 5, SPR_CTR), X(19, 528, 20, 0, 0)});
  polymorphic.initial.gpr[5] = polymorphic.initial.gpr[7] = target | 1;
  polymorphic.initial.gpr[6] = other_target | 3;
  polymorphic.initial.gpr[10] = 0;
  polymorphic.extra = {
      {target, target & 0x1fffffff,
       {X(31, 444, 6, 5, 6), D(14, 10, 10, 1), Branch(static_cast<s32>(CODE - (target + 8)))}},
      {other_target, other_target & 0x1fffffff,
       {X(31, 444, 7, 5, 7), D(11, 0, 10, 12), D(16, 12, 0, CODE - (other_target + 8)),
        Branch(static_cast<s32>(STOP - (other_target + 12)))}}};
  polymorphic.step_limit = 256;
  const u32 remote = CODE + 0x400c; // collides with CODE in the 4096-entry target cache
  auto& collision = c.Add("indirect PC hash collision and hop/slice exits",
                          {D(14, 8, 8, 1), Spr(467, 6, SPR_CTR), X(19, 528, 20, 0, 0)});
  collision.initial.gpr[6] = remote | 3;
  collision.initial.gpr[8] = 0;
  collision.initial.lr = CODE | 1;
  collision.extra = {
      {remote,
       remote & 0x1fffffff,
       {D(11, 0, 8, 5000), X(19, 16, 12, 0, 0), Branch(static_cast<s32>(STOP - (remote + 8)))}}};
  collision.step_limit = 30000;
  collision.short_slice = true;
  c.group = "reset and state reload";
  auto& refresh =
      c.Add("reset, CPU state reload, core return, FPRF cache refresh", {X(59, 21, 8, 1, 2)});
  refresh.initial.fpscr = FPRF_MASK;
  refresh.lifecycle = true;
}

std::vector<Program> Directed(bool wii, bool enables, Mode mode)
{
  Corpus c(wii);
  c.Add("finite NEON smoke", {X(4, 21, 8, 1, 2), X(4, 20, 9, 8, 1)});
  c.group = "integer";
  IntegerCases(c);
  c.group = "FP pairs";
  FPPairCases(c);
  c.group = "branches";
  BranchCases(c, enables);
  c.group = "arithmetic";
  ArithmeticCases(c, enables);
  c.group = "memory";
  MemoryCases(c, mode == Mode::Checked);
  c.group = "FIFO";
  FifoCases(c);
  c.group = "feature keys";
  FeatureCases(c);
  for (auto& p : c.cases)
  {
    const u32 pc = p.initial.pc + static_cast<u32>(4 * p.words.size());
    p.words.push_back(Branch(static_cast<s32>(p.stop - pc)));
  }
  return std::move(c.cases);
}

void InstallBATs(Core::System& system)
{
  ASSERT_NE(system.GetMemory().GetRAM(), nullptr);
  if (system.IsWii())
    ASSERT_NE(system.GetMemory().GetEXRAM(), nullptr);
  // CBoot::SetupBAT is private. These are its OS mappings, including the W/I mirror.
  auto& s = system.GetPPCState();
  s.spr[SPR_IBAT0U] = s.spr[SPR_DBAT0U] = 0x80001fff;
  s.spr[SPR_IBAT0L] = s.spr[SPR_DBAT0L] = 0x00000002;
  s.spr[SPR_DBAT1U] = 0xc0001fff;
  s.spr[SPR_DBAT1L] = 0x0000002a;
  // Identity-map the low page for absolute branches and literal rA=0 accesses.
  s.spr[SPR_IBAT2U] = s.spr[SPR_DBAT2U] = 2;
  s.spr[SPR_IBAT2L] = s.spr[SPR_DBAT2L] = 2;
  s.spr[SPR_IBAT3U] = s.spr[SPR_IBAT3L] = 0;
  s.spr[SPR_SDR] = PAGE_TABLE;
  const std::array<u8, 64 * 1024> empty_page_table{};
  system.GetMemory().CopyToEmu(PAGE_TABLE, empty_page_table.data(), empty_page_table.size());
  // SR3.VSID=0, page index 1, API=0. A non-BAT alias forces a legal cold memory access
  // while retaining register/address overlap. Both executors own the same physical scratch.
  system.GetMemory().Write_U32(0x80000000, PAGE_TABLE + 64);
  system.GetMemory().Write_U32((system.IsWii() ? MEM2_DATA : MEM1_DATA) | 2, PAGE_TABLE + 68);
  system.GetMemory().Write_U32(0x80000000, PAGE_TABLE + 3 * 64);
  system.GetMemory().Write_U32((system.IsWii() ? MEM2_DATA : MEM1_DATA) | 2,
                               PAGE_TABLE + 3 * 64 + 4);
  system.GetMemory().Write_U32(0x80000000, PAGE_TABLE + 4 * 64);
  system.GetMemory().Write_U32(GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS | 0x2a,
                               PAGE_TABLE + 4 * 64 + 4);
  system.GetMemory().Write_U32(0x80000000, PAGE_TABLE + 6 * 64);
  system.GetMemory().Write_U32((system.IsWii() ? MEM2_DATA : MEM1_DATA) | 2,
                               PAGE_TABLE + 6 * 64 + 4);
  system.GetMMU().SDRUpdated();
  if (system.IsWii())
  {
    HID4(s).SBE = 1;
    s.spr[SPR_IBAT4U] = s.spr[SPR_DBAT4U] = 0x90001fff;
    s.spr[SPR_IBAT4L] = s.spr[SPR_DBAT4L] = 0x10000002;
    s.spr[SPR_DBAT5U] = 0xd0001fff;
    s.spr[SPR_DBAT5L] = 0x1000002a;
  }
  HID2(s).PSE = 1;
  HID2(s).LSQE = 1;
  HID0(s).DCE = 1; // dcbz is legal even with host data-cache emulation disabled.
  system.GetMMU().DBATUpdated();
  system.GetMMU().IBATUpdated();
}

void Install(Core::System& system, const Program& program)
{
  auto& cpu = system.GetPPCState();
  const auto& initial = program.initial;
  std::ranges::copy(initial.gpr, cpu.gpr);
  for (size_t i = 0; i < initial.ps.size(); ++i)
    cpu.ps[i].SetBoth(initial.ps[i][0], initial.ps[i][1]);
  for (size_t i = 0; i < initial.gqr.size(); ++i)
    GQR(cpu, i) = initial.gqr[i];
  HID2(cpu).LSQE = program.lsqe;
  cpu.spr[SPR_DAR] = initial.dar;
  cpu.spr[SPR_DSISR] = initial.dsisr;
  cpu.spr[SPR_MMCR0] = program.monitor ? (1 << 6) | 11 : 0;
  cpu.spr[SPR_MMCR1] = program.monitor ? (11 << 27) | (1 << 22) : 0;
  for (size_t i = 0; i < std::size(PMC_REGS); ++i)
    cpu.spr[PMC_REGS[i]] = initial.pmc[i];
  PowerPC::MMCRUpdated(cpu);
  cpu.cr.Set(initial.cr);
  cpu.SetXER(UReg_XER{initial.xer});
  LR(cpu) = initial.lr;
  CTR(cpu) = initial.ctr;
  SRR0(cpu) = initial.srr0;
  SRR1(cpu) = initial.srr1;
  cpu.pc = initial.pc;
  cpu.npc = cpu.pc + 4;
  cpu.Exceptions = initial.exceptions;
  cpu.reserve = initial.reserve;
  cpu.reserve_address = initial.reserve_address;
  cpu.msr.Hex = initial.msr;
  system.GetPowerPC().MSRUpdated();
  const u32 expected_key = ((initial.msr >> 4) & 3) | (program.monitor ? 4u : 0u) |
                           ((initial.msr & 0x2000) ? 8u : 0u);
  EXPECT_EQ(cpu.feature_flags, expected_key);
  cpu.fpscr.Hex = initial.fpscr;
  PowerPC::RoundingModeUpdated(cpu);

  auto& memory = system.GetMemory();
  if (program.paged_code)
  {
    memory.Write_U32(0x80000000, PAGE_TABLE + 128);
    memory.Write_U32(0x40002, PAGE_TABLE + 132);
    system.GetMMU().InvalidateTLBEntry(0x30002000);
  }
  memory.CopyToEmu(program.physical_data, initial.data.data(), initial.data.size());
  auto write_code = [&](u32 pc, u32 pa, const std::vector<u32>& words) {
    for (size_t i = 0; i < words.size(); ++i)
      memory.Write_U32(words[i], pa + 4 * i);
    const u32 size = static_cast<u32>(4 * words.size());
    const u32 length = std::max(128u, ((pc & 31) + size + 31) & ~31u);
    system.GetJitInterface().InvalidateICache(pc & ~31u, length, false);
  };
  write_code(initial.pc, initial.pc & 0x1fffffff, program.words);
  for (const auto& seg : program.extra)
  {
    // A second physical body may not yet be mapped at its eventual execution address.
    const u32 alias = seg.physical | (cpu.msr.IR ? 0x80000000 : 0);
    system.GetJitInterface().InvalidateICache(
        alias & ~31u, ((alias & 31) + static_cast<u32>(4 * seg.words.size()) + 31) & ~31u, false);
    write_code(seg.pc, seg.physical, seg.words);
  }
  if (program.fifo_prefix >= 0)
  {
    auto& pi = system.GetProcessorInterface();
    pi.m_fifo_cpu_base = pi.m_fifo_cpu_write_pointer = FIFO_RAM;
    pi.m_fifo_cpu_end = FIFO_RAM + FIFO_CAPACITY - GPFifo::GATHER_PIPE_SIZE;
    std::array<u8, FIFO_CAPACITY> empty{};
    memory.CopyToEmu(FIFO_RAM, empty.data(), empty.size());
    auto& gp = system.GetGPFifo();
    gp.Init();
    for (int i = 0; i < program.fifo_prefix; ++i)
      gp.Write8(static_cast<u8>(0xa0 + i));
  }
}

State Capture(Core::System& system, const Program& p)
{
  const auto& cpu = system.GetPPCState();
  State s;
  std::ranges::copy(cpu.gpr, s.gpr.begin());
  for (size_t i = 0; i < s.ps.size(); ++i)
    s.ps[i] = {cpu.ps[i].PS0AsU64(), cpu.ps[i].PS1AsU64()};
  s.cr = cpu.cr.Get();
  s.xer = cpu.GetXER().Hex;
  s.fpscr = cpu.fpscr.Hex;
  s.lr = LR(cpu);
  s.ctr = CTR(cpu);
  s.srr0 = SRR0(cpu);
  s.srr1 = SRR1(cpu);
  s.msr = cpu.msr.Hex;
  s.pc = cpu.pc;
  s.npc = cpu.npc;
  s.dar = cpu.spr[SPR_DAR];
  s.dsisr = cpu.spr[SPR_DSISR];
  for (size_t i = 0; i < s.gqr.size(); ++i)
    s.gqr[i] = GQR(cpu, i);
  for (size_t i = 0; i < s.pmc.size(); ++i)
    s.pmc[i] = cpu.spr[PMC_REGS[i]];
  s.exceptions = cpu.Exceptions;
  s.reserve = cpu.reserve;
  s.reserve_address = cpu.reserve ? cpu.reserve_address : 0;
  system.GetMemory().CopyFromEmu(s.data.data(), p.physical_data, s.data.size());
  if (p.fifo_prefix >= 0)
  {
    s.fifo_committed = system.GetProcessorInterface().m_fifo_cpu_write_pointer - FIFO_RAM;
    const auto pending = cpu.gather_pipe_ptr - cpu.gather_pipe_base_ptr;
    EXPECT_LT(s.fifo_committed, FIFO_CAPACITY);
    EXPECT_GE(pending, 0);
    EXPECT_LT(pending, GPFifo::GATHER_PIPE_SIZE);
    if (s.fifo_committed < FIFO_CAPACITY && pending >= 0 && pending < GPFifo::GATHER_PIPE_SIZE)
    {
      s.fifo.resize(s.fifo_committed);
      if (s.fifo_committed)
        system.GetMemory().CopyFromEmu(s.fifo.data(), FIFO_RAM, s.fifo.size());
      s.fifo.insert(s.fifo.end(), cpu.gather_pipe_base_ptr, cpu.gather_pipe_ptr);
    }
  }
  return s;
}

class Executor
{
public:
  virtual ~Executor() = default;
  virtual std::string_view Name() const = 0;
  virtual State Run(const Program& program) = 0;
};

class DolphinExecutor final : public Executor
{
public:
  DolphinExecutor(Core::System& system, PowerPC::CPUCore core, bool fprf, bool enables, Mode mode,
                  bool init_mmio = false)
      : m_system(system), m_cached(core == PowerPC::CPUCore::CachedInterpreter),
        m_check_records(m_cached && mode == Mode::Normal && !enables)
  {
    Config::SetCurrent(Config::MAIN_CPU_CORE, core);
    Config::SetCurrent(Config::MAIN_FPRF, fprf);
    Config::SetCurrent(Config::MAIN_FLOAT_EXCEPTIONS, enables);
    Config::SetCurrent(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, enables);
    Config::SetCurrent(Config::MAIN_CIR_PS_NEON, m_cached && mode != Mode::NoPIC);
    Config::SetCurrent(Config::MAIN_CIR_PSQ_FASTPATH, m_cached);
    // PSQ is a shared handler with a process-global switch. The plain Interpreter doesn't reset it.
    PowerPC::SetPsqFastpathEnabled(false);
    system.GetCoreTiming().Init();
    if (init_mmio)
    {
      // InitMMIO registers EXI channels as well as PI. Give EXI an in-memory SRAM override.
      system.GetProcessorInterface().Init();
      m_saved_sram = system.GetSRAM();
      system.GetExpansionInterface().Init(&*m_saved_sram);
    }
    system.GetMemory().Init();
    m_slice_event = system.GetCoreTiming().RegisterEvent(
        "CpuDiff slice", [this](Core::System&, u64, s64) { m_slice_fired = true; });
    system.GetPowerPC().Init(core);
    system.GetCommandProcessor().Init(); // GP link and read disabled, no video consumer
    if (init_mmio)
      system.GetMemory().InitMMIO(system);
    if (!m_cached)
      PowerPC::g_fprf_updates_off = !fprf;
    InstallBATs(system);
  }

  ~DolphinExecutor() override
  {
    m_system.GetPowerPC().Shutdown();
    m_system.GetMemory().Shutdown();
    if (m_saved_sram)
    {
      m_system.GetExpansionInterface().Shutdown();
      m_system.GetSRAM() = *m_saved_sram;
    }
    m_system.GetCoreTiming().Shutdown();
    PowerPC::g_fprf_updates_off = false;
    PowerPC::SetPsqFastpathEnabled(false);
  }

  std::string_view Name() const override { return m_system.GetPowerPC().GetCPUName(); }

  State Run(const Program& program) override
  {
    Common::ScopeGuard restore_fp([] { Common::FPU::LoadSIMDState(); });
    Install(m_system, program);
    m_slice_fired = false;
    if (program.short_slice)
      m_system.GetCoreTiming().ScheduleEvent(127, m_slice_event);
    if (program.breakpoint)
    {
      const bool old_debug = Config::Get(Config::MAIN_ENABLE_DEBUGGING);
      Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);
      auto& bp = m_system.GetPowerPC().GetBreakPoints();
      const u32 branch = program.initial.pc + 4;
      bp.Add(branch, false, false, std::nullopt); // emit a check, then let the branch run
      Common::ScopeGuard remove([&] {
        bp.Remove(branch);
        Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, old_debug);
      });
      return ExecuteInstalled(program);
    }
    if (program.lifecycle)
      return RunAcrossCacheClears(program);
    if (!program.expected_alert.empty())
    {
      State result;
      EXPECT_NONFATAL_FAILURE(result = ExecuteInstalled(program), program.expected_alert);
      return result;
    }
    return ExecuteInstalled(program);
  }

private:
  State RunAcrossCacheClears(const Program& program)
  {
    // Save the CPU section used by savestates before warming this PC. Read it after reset.
    auto& ppc = m_system.GetPowerPC();
    auto& cpu = m_system.GetPPCState();
    std::vector<u8> snapshot(1024 * 1024);
    u8* cursor = snapshot.data();
    PointerWrap writer(&cursor, snapshot.size(), PointerWrap::Mode::Write);
    ppc.DoState(writer);
    if (!writer.IsWriteMode())
    {
      ADD_FAILURE() << "CPU snapshot exceeded " << snapshot.size() << " bytes";
      return {};
    }
    snapshot.resize(writer.GetOffsetFromPreviousPosition(snapshot.data()));
    const u32 key = cpu.feature_flags;
    const State before = ExecuteInstalled(program);
    if (m_cached)
    {
      Core::CPUThreadGuard guard(m_system);
      auto& jit = m_system.GetJitInterface();
      for (int i = 0; i < 3; ++i)
      {
        const JitBlock* entry = nullptr;
        jit.RunOnBlocks(guard, [&](const JitBlock& block) {
          if (block.effectiveAddress == program.initial.pc && block.feature_flags == key)
            entry = &block;
        });
        if (!entry)
        {
          ADD_FAILURE() << "Missing warm block before single-block erase";
          break;
        }
        jit.EraseSingleBlock(*entry);
        cpu.pc = program.initial.pc;
        cpu.npc = cpu.pc + 4;
        cpu.fpscr.Hex = program.initial.fpscr;
        EXPECT_TRUE(ExecuteInstalled(program) == before);
      }
    }
    ppc.Reset();
    EXPECT_EQ(cpu.feature_flags, 0);
    if (m_cached)
    {
      InstallBATs(m_system);
      Install(m_system, program);
      ExecuteInstalled(program);
      EXPECT_GT(m_system.GetJitInterface().GetBlockCount(), 0u);
    }
    cursor = snapshot.data();
    PointerWrap reader(&cursor, snapshot.size(), PointerWrap::Mode::Read);
    ppc.DoState(reader);
    EXPECT_TRUE(reader.IsReadMode());
    EXPECT_EQ(cursor, snapshot.data() + snapshot.size());
    EXPECT_EQ(cpu.feature_flags, key);
    EXPECT_EQ(m_system.GetJitInterface().GetBlockCount(), 0u);
    if (m_cached)
    {
      ppc.SetMode(PowerPC::CoreMode::Interpreter);
      cpu.msr.FP = 0;
      ppc.SetMode(PowerPC::CoreMode::JIT);
      EXPECT_EQ(cpu.feature_flags, key & ~FEATURE_FLAG_MSR_FP);
      cpu.msr.FP = 1;
      ppc.MSRUpdated();
    }
    const State restored = ExecuteInstalled(program);
    EXPECT_TRUE(before == restored) << "replaying the restored CPU state";
    const bool original = Config::Get(Config::MAIN_FPRF);
    for (bool policy : {!original, original})
    {
      // Leave the tape warm and exercise the normal config notification and cache clear.
      Config::SetCurrent(Config::MAIN_FPRF, policy);
      if (!m_cached)
        PowerPC::g_fprf_updates_off = !policy;
      cpu.pc = program.initial.pc;
      cpu.npc = cpu.pc + 4;
      cpu.fpscr.Hex = program.initial.fpscr;
      const auto result = ExecuteInstalled(program);
      EXPECT_EQ(PowerPC::g_fprf_updates_off, !policy);
      EXPECT_EQ((result.fpscr & FPRF_MASK) != FPRF_MASK, policy);
    }
    return restored;
  }

  State ExecuteInstalled(const Program& program)
  {
    auto in_program = [&](u32 pc) {
      auto inside = [pc](u32 start, size_t words) {
        return !(pc & 3) && pc >= start && (pc - start) / 4 < words;
      };
      return inside(program.initial.pc, program.words.size()) ||
             std::ranges::any_of(program.extra,
                                 [&](const auto& s) { return inside(s.pc, s.words.size()); });
    };
    auto& cpu = m_system.GetPPCState();
    u32 cycles = 0;
    bool saw_block = false;
    bool invalidated = false;
    std::vector<u32> fault_cycles;
    for (u32 step = 0; step < program.step_limit && in_program(cpu.pc); ++step)
    {
      const u32 pc = cpu.pc;
      // The public Interpreter single-step deliberately resets downcount to zero. Recover its
      // documented guest charge from the fetched opcode table for the directed timing cases.
      if (!m_cached && !program.label.empty())
        cycles += PPCTables::GetOpInfo(m_system.GetMMU().Read_Opcode(pc), pc)->num_cycles;
      m_system.GetPowerPC().SingleStep();
      if (m_cached)
      {
        saw_block |= m_system.GetJitInterface().GetBlockCount() != 0;
        cycles += m_system.GetCoreTiming().GetGlobals().slice_length - cpu.downcount;
        if (program.invalidate_caller_at && !invalidated && cpu.pc == program.invalidate_caller_at)
        {
          m_system.GetJitInterface().InvalidateICache(program.initial.pc, 32, false);
          invalidated = true;
        }
      }
      if (program.lazy_fpu && pc != FPU_VECTOR && cpu.pc == FPU_VECTOR)
      {
        EXPECT_EQ(SRR0(cpu), program.fault_pc);
        EXPECT_EQ(SRR1(cpu) & 0x2000, 0u);
        fault_cycles.push_back(cycles);
      }
    }
    if (m_cached)
    {
      EXPECT_TRUE(saw_block); // a FIFO write or icbi can erase the last block during execution
      EXPECT_GT(cycles, 0u);  // a compile-only ExecuteOneBlock miss consumes zero guest cycles
      if (program.invalidate_caller_at)
        EXPECT_TRUE(invalidated);
    }
    State result = Capture(m_system, program);
    if (m_check_records && !program.expected_record.empty())
    {
      Core::CPUThreadGuard guard(m_system);
      std::ostringstream tape;
      auto& jit = m_system.GetJitInterface();
      jit.RunOnBlocks(guard, [&](const JitBlock& block) {
        if (block.effectiveAddress == program.initial.pc)
          jit.DisassembleNearCode(block, tape);
      });
      EXPECT_NE(tape.str().find(program.expected_record), std::string::npos) << tape.str();
    }
    if (program.short_slice)
      EXPECT_TRUE(m_slice_fired);
    if (!program.label.empty())
      result.cycles = cycles;
    if (program.lazy_fpu)
    {
      EXPECT_EQ(fault_cycles.size(), 2u);
      EXPECT_EQ(result.gpr[20], 2u);
      EXPECT_EQ(result.gpr[22], 1u);
      result.fault_cycles = std::move(fault_cycles);
      if (m_cached)
      {
        Core::CPUThreadGuard guard(m_system);
        bool fp_on = false, fp_off = false;
        m_system.GetJitInterface().RunOnBlocks(guard, [&](const JitBlock& block) {
          if (block.effectiveAddress == program.initial.pc + 28)
          {
            if (block.feature_flags & FEATURE_FLAG_MSR_FP)
              fp_on = true;
            else
              fp_off = true;
          }
        });
        EXPECT_TRUE(fp_on && fp_off) << "Both FP keys must coexist for the common function entry";
      }
    }
    if (program.remap)
      InstallBATs(m_system);
    return result;
  }

  Core::System& m_system;
  bool m_cached;
  bool m_check_records;
  std::optional<Sram> m_saved_sram;
  CoreTiming::EventType* m_slice_event = nullptr;
  bool m_slice_fired = false;
};

std::string Listing(const Program& p)
{
  std::string text;
  auto append = [&](u32 start, const std::vector<u32>& words) {
    for (size_t i = 0; i < words.size(); ++i)
    {
      const u32 pc = start + static_cast<u32>(4 * i);
      text += fmt::format("{:08x}  {:08x}  {}\n", pc, words[i],
                          Common::GekkoDisassembler::Disassemble(words[i], pc));
    }
  };
  append(p.initial.pc, p.words);
  for (const auto& seg : p.extra)
    append(seg.pc, seg.words);
  return text;
}

std::string Differences(const State& ref, const State& got, u32 physical_data,
                        std::string_view candidate)
{
  std::string text = fmt::format("{:<21}  {:>16}  {:>16}\n", "field", "Interpreter", candidate);
  auto field = [&](std::string_view name, u64 a, u64 b) {
    if (a != b)
      text += fmt::format("{:<21}  {:016x}  {:016x}\n", name, a, b);
  };
  for (size_t i = 0; i < ref.gpr.size(); ++i)
  {
    field(fmt::format("r{}", i), ref.gpr[i], got.gpr[i]);
    for (size_t j = 0; j < 2; ++j)
      field(fmt::format("f{}.PS{}", i, j), ref.ps[i][j], got.ps[i][j]);
  }
  field("CR", ref.cr, got.cr);
  field("XER", ref.xer, got.xer);
  field("FPSCR", ref.fpscr, got.fpscr);
  field("LR", ref.lr, got.lr);
  field("CTR", ref.ctr, got.ctr);
  field("MSR", ref.msr, got.msr);
  field("PC", ref.pc, got.pc);
  field("NPC", ref.npc, got.npc);
  field("guest cycles", ref.cycles, got.cycles);
  field("DAR", ref.dar, got.dar);
  field("DSISR", ref.dsisr, got.dsisr);
  for (size_t i = 0; i < ref.gqr.size(); ++i)
    field(fmt::format("GQR{}", i), ref.gqr[i], got.gqr[i]);
  for (size_t i = 0; i < ref.pmc.size(); ++i)
    field(fmt::format("PMC{}", i + 1), ref.pmc[i], got.pmc[i]);
  field("FIFO committed", ref.fifo_committed, got.fifo_committed);
  field("FIFO size", ref.fifo.size(), got.fifo.size());
  for (size_t i = 0; i < std::min(ref.fifo.size(), got.fifo.size()); ++i)
    field(fmt::format("FIFO[{}]", i), ref.fifo[i], got.fifo[i]);
  field("lazy FPU visits", ref.fault_cycles.size(), got.fault_cycles.size());
  for (size_t i = 0; i < std::min(ref.fault_cycles.size(), got.fault_cycles.size()); ++i)
    field(fmt::format("cycles at FPU #{}", i + 1), ref.fault_cycles[i], got.fault_cycles[i]);
  field("SRR0", ref.srr0, got.srr0);
  field("SRR1", ref.srr1, got.srr1);
  field("Exceptions", ref.exceptions, got.exceptions);
  field("reserve", ref.reserve, got.reserve);
  field("reserve_address", ref.reserve_address, got.reserve_address);
  for (size_t i = 0; i < ref.data.size(); ++i)
  {
    if (ref.data[i] != got.data[i])
      field(fmt::format("RAM[{:#010x}]", physical_data + i), ref.data[i], got.data[i]);
  }
  return text;
}

bool FPProgramException(const State& s)
{
  constexpr u32 causes = 0x001e0000;
  return s.pc == PROGRAM_VECTOR && s.npc == PROGRAM_VECTOR && s.exceptions == 0 &&
         (s.srr1 & causes) == static_cast<u32>(ProgramExceptionCause::FloatingPoint) &&
         (s.srr1 & 0x900) && (s.fpscr & FPSCR_FEX);
}

bool ValidExit(const Program& p, const State& s, bool enables)
{
  if (s.exceptions != 0)
    return false;
  if (p.required_vector)
    return s.pc == p.required_vector && s.srr0 == p.fault_pc &&
           (p.required_vector != PROGRAM_VECTOR ||
            (s.srr1 & 0x001e0000) == static_cast<u32>(ProgramExceptionCause::IllegalInstruction));
  if (s.pc == PROGRAM_VECTOR)
    return enables && FPProgramException(s);
  if (p.fifo_prefix >= 0)
  {
    const u32 bytes = static_cast<u32>(p.fifo_prefix) + p.fifo_store_bytes;
    if (s.fifo.size() != bytes || s.fifo_committed != bytes / 32 * 32)
      return false;
    if (!p.expected_store.empty() && !std::equal(p.expected_store.begin(), p.expected_store.end(),
                                                 s.fifo.begin() + p.fifo_prefix))
      return false;
  }
  return !p.expects_trap && s.pc == p.stop;
}

bool ParseU32(std::string_view value, u32& result)
{
  const bool hex = value.starts_with("0x") || value.starts_with("0X");
  if (hex)
    value.remove_prefix(2);
  const char* end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(value.data(), end, result, hex ? 16 : 10);
  return ptr == end && ec == std::errc{};
}

void Compare(Executor& candidate, const Program& p, const State& ref, u32 seed, bool fprf,
             bool enables, u32& accepted, bool cached_srr0_quirk = false)
{
  State got = candidate.Run(p);
  const u32 actual_srr0 = got.srr0;
  const bool require_fp_offset =
      cached_srr0_quirk && enables && FPProgramException(ref) && FPProgramException(got);
  if (require_fp_offset)
  {
    ASSERT_EQ(u64{ref.srr0}, u64{got.srr0} + 4)
        << fmt::format("FP program exception SRR0 Interpreter={:08x} {}={:08x}\n{}",
                       ref.srr0, candidate.Name(), got.srr0, Listing(p));
    got.srr0 = ref.srr0;
  }
  if (!ValidExit(p, ref, enables) || !ValidExit(p, got, enables) || got != ref)
  {
    got.srr0 = actual_srr0;
    FAIL() << fmt::format("{} {}={} FPRF={} FP enables={}\n"
                          "Exit PCs Interpreter={:08x} {}={:08x}, "
                          "stop={:08x}, FP trap required={}, dispatch cap={}\n"
                          "SRR1={:08x}/{:08x}, pending exceptions={:08x}/{:08x}\n"
                          "Required vector={:08x} fault PC={:08x}, "
                          "FIFO prefix={} store bytes={} observed lengths={}/{}\n{}{}",
                          p.label, p.label.empty() ? "PH_CPUDIFF_SEED" : "PH_CPUDIFF_DIRECTED_CASE",
                          seed, fprf, enables, ref.pc, candidate.Name(), got.pc, p.stop,
                          p.expects_trap, p.step_limit, ref.srr1, got.srr1, ref.exceptions,
                          got.exceptions, p.required_vector, p.fault_pc, p.fifo_prefix,
                          p.fifo_store_bytes, ref.fifo.size(), got.fifo.size(), Listing(p),
                          Differences(ref, got, p.physical_data, candidate.Name()));
  }
  accepted += require_fp_offset;
}

void CheckDirectedProfile(Mode mode)
{
  const std::string report = CIRProfiler::BuildHotBlocksReport(0);
  auto counter = [&](std::string_view name) -> u64 {
    const size_t start = report.find(name);
    u64 value = 0;
    if (start != std::string::npos)
      std::from_chars(report.data() + start + name.size(), report.data() + report.size(), value);
    return value;
  };
  EXPECT_GT(counter("total_block_runs="), 0u) << report;
  if (mode == Mode::Optimized)
  {
    EXPECT_GT(counter("gp_fused_runs="), 0u) << report;
    EXPECT_GT(counter("global target cache: "), 0u) << report;
  }
#if defined(_M_ARM_64) || defined(__aarch64__)
  if (mode != Mode::NoPIC)
  {
    std::istringstream rows(Interpreter::BuildPsNeonReport());
    std::string line;
    double hit_percent = 0;
    while (std::getline(rows, line))
    {
      std::istringstream fields(line);
      std::string op;
      u64 tries;
      if (fields >> op >> tries && op == "ps_add")
        fields >> hit_percent;
    }
    EXPECT_GT(hit_percent, 0.0) << Interpreter::BuildPsNeonReport();
  }
#endif
}

void CheckNativeMSR(Core::System& system)
{
  for (auto core : PowerPC::AvailableCPUCores())
  {
    if (core != PowerPC::CPUCore::JIT64 && core != PowerPC::CPUCore::JITARM64)
      continue;
    // Both native dispatchers check CPU::State between unlinked blocks in debug mode.
    // The first block keeps "li; mtmsr" adjacent for the constant-source emitter. The
    // second takes its MSR value from a live register. No other native op family is required.
    Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, true);
    Common::ScopeGuard restore([&] { Config::SetCurrent(Config::MAIN_ENABLE_DEBUGGING, false); });
    Common::ScopeGuard restore_fp([] { Common::FPU::LoadSIMDState(); });
    DolphinExecutor native(system, core, true, false, Mode::Normal);
    ASSERT_STREQ(system.GetPowerPC().GetCPUName(),
                 core == PowerPC::CPUCore::JIT64 ? "JIT64" : "JITARM64");
    Program p = Generate(1, false, false);
    p.words = {D(14, 5, 0, 0x32), X(31, 146, 5, 0, 0), X(31, 146, 6, 0, 0), 0x44000002,
               Branch(static_cast<s32>(STOP - (CODE + 16)))};
    p.extra = {{SC_VECTOR, SC_VECTOR, {0x4c000064}}, {STOP, STOP & 0x1fffffff, {Branch(0)}}};
    p.initial.gpr[6] = 0x2032;
    p.monitor = true;
    Install(system, p);
    auto& cpu = system.GetPPCState();
    for (const auto& [pc, msr] : {std::pair{CODE + 8, 0x32u}, std::pair{CODE + 12, 0x2032u},
                                  std::pair{SC_VECTOR, 0u}, std::pair{CODE + 16, 0x2032u}})
    {
      for (u32 i = 0; cpu.pc != pc && i < 8; ++i)
        system.GetPowerPC().SingleStep();
      ASSERT_EQ(cpu.pc, pc) << native.Name();
      EXPECT_EQ(cpu.msr.Hex & 0x2030, msr & 0x2030);
      EXPECT_EQ(cpu.feature_flags, MSRFeatureFlags(msr) | FEATURE_FLAG_PERFMON);
    }
  }
}

void RunCases(bool wii, bool enables, bool fprf, Mode mode = Mode::Normal, bool directed = false)
{
  u32 first = 0;
  u32 count = 2048;
  if (const char* replay = std::getenv("PH_CPUDIFF_SEED"); replay && *replay)
  {
    ASSERT_TRUE(ParseU32(replay, first)) << "Invalid PH_CPUDIFF_SEED: " << replay;
    count = 1;
  }
  else if (const char* cases = std::getenv("PH_CPUDIFF_CASES"); cases && *cases)
  {
    ASSERT_TRUE(ParseU32(cases, count) && count > 0) << "Invalid PH_CPUDIFF_CASES: " << cases;
  }

  const std::string profile = File::CreateTempDir();
  ASSERT_FALSE(profile.empty());
  Core::DeclareAsCPUThread();
  Common::FPU::SaveSIMDState();
  UICommon::SetUserDirectory(profile);
  Config::Init();
  SConfig::Init();
  Common::ScopeGuard config_guard([&] {
    SConfig::Shutdown();
    Config::Shutdown();
    Common::FPU::LoadSIMDState();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(profile);
  });
  auto& system = Core::System::GetInstance();
  Config::SetCurrent(Config::MAIN_MMU, mode == Mode::Checked);
  Config::SetCurrent(Config::MAIN_OVERCLOCK_ENABLE, false);
  Config::SetCurrent(Config::MAIN_CPU_THREAD, false);
  Config::SetCurrent(Config::MAIN_ACCURATE_CPU_CACHE, false);
  Config::SetCurrent(Config::MAIN_PAUSE_ON_PANIC, false);
  Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false);     // CIR uses Memory's BAT page pointers.
  Config::SetCurrent(Config::MAIN_JIT_FOLLOW_BRANCH, false); // observe the terminal b before STOP
  Config::SetCurrent(Config::MAIN_CIR_LONG_BLOCKS, true);
  Config::SetCurrent(Config::MAIN_CIR_PIC_LOADSTORE, mode != Mode::NoPIC);
  Config::SetCurrent(Config::MAIN_CIR_RECORD_CHAINING, mode != Mode::NoPIC);
  Config::SetCurrent(Config::MAIN_CIR_BLOCK_LINKING, mode != Mode::Checked);
  Config::SetCurrent(Config::MAIN_CIR_BLOCK_LINKING_VALIDATE, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_TAIL_LINK, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_DYN_LINKING, true);
  Config::SetCurrent(Config::MAIN_CIR_DYN_TARGET_CACHE, mode != Mode::NoPIC);
  Config::SetCurrent(Config::MAIN_CIR_GP_COPY_FUSION, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_DEAD_FLAG_ELIM, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_DEAD_FPRF_ELIM, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_CACHE_LOOP_FF, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_STORE_LOOP_FF, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_PS_NEON_VALIDATE, mode == Mode::Optimized);
  Config::SetCurrent(Config::MAIN_CIR_MICROOP_FUSION_VALIDATE, mode == Mode::NoPIC);
  Config::SetCurrent(Config::MAIN_CIR_PROFILE, directed);
  Config::SetCurrent(Config::MAIN_CIR_MEM_MICROOPS, true);
  Config::SetCurrent(Config::MAIN_CIR_MICROOP_FUSION, true);
  Config::SetCurrent(Config::MAIN_CIR_MICRO_PAIRS, true);
  Config::SetCurrent(Config::MAIN_CIR_SPECIALIZED_OPS, true);
  Config::SetCurrent(Config::MAIN_CIR_SPECIALIZED_FP_ARITH, true);
  Config::SetCurrent(Config::MAIN_CIR_SPECIALIZED_FP_LS, true);
  Config::SetCurrent(Config::MAIN_CIR_SPECIALIZED_PSQ, true);
  system.Initialize();
  const bool old_wii = system.IsWii();
  system.SetIsWii(wii);
  Common::ScopeGuard system_guard([&] {
    system.SetIsWii(old_wii);
    Config::ClearCurrentRunLayer();
    system.Initialize();
  });

  // Batching amortizes real core initialization while bounding the oracle's memory usage.
  // Each batch/engine gets a fresh CoreTiming event queue and an empty translation cache.
  std::vector<Program> corpus;
  bool selected_case = false;
  if (directed)
  {
    for (const auto slot : ExpansionInterface::SLOTS)
      Config::SetCurrent(Config::GetInfoForEXIDevice(slot), ExpansionInterface::EXIDeviceType::None);
    corpus = Directed(wii, enables, mode);
    count = static_cast<u32>(corpus.size());
    first = 0;
    if (const char* id = std::getenv("PH_CPUDIFF_DIRECTED_CASE"); id && *id)
    {
      ASSERT_TRUE(ParseU32(id, first) && first < count) << "Invalid directed case: " << id;
      count = 1;
      selected_case = true;
    }
  }
  u32 accepted = 0;
  Common::ScopeGuard report_count([&] {
    fmt::print("Accepted FP program-exception SRR0 differences: {}\n", accepted);
    testing::Test::RecordProperty("accepted_fp_srr0", std::to_string(accepted));
  });
  const u32 batch_size = directed ? count : 2048;
  for (u64 offset = 0; offset < count; offset += batch_size)
  {
    const u32 start = first + static_cast<u32>(offset);
    const u32 batch_count = std::min(batch_size, count - static_cast<u32>(offset));
    std::vector<u32> seeds;
    seeds.reserve(batch_count);
    for (u32 i = 0; i < batch_count; ++i)
    {
      const u32 seed = start + i;
      if (directed && !corpus[seed].known_divergence.empty())
      {
        const auto& p = corpus[seed];
        const auto message = fmt::format("known_divergence PH_CPUDIFF_DIRECTED_CASE={} {}: {}",
                                         seed, p.label, p.known_divergence);
        if (selected_case)
          GTEST_SKIP() << message;
        fmt::print("{}\n", message);
        continue;
      }
      seeds.push_back(seed);
    }
    const u32 n = static_cast<u32>(seeds.size());
    auto program = [&](u32 seed) { return directed ? corpus[seed] : Generate(seed, wii, enables); };
    std::vector<State> reference;
    reference.reserve(n);
    {
      DolphinExecutor oracle(system, PowerPC::CPUCore::Interpreter, fprf, enables, mode, directed);
      ASSERT_STREQ(system.GetPowerPC().GetCPUName(), system.GetInterpreter().GetName());
      ASSERT_EQ(PowerPC::g_fprf_updates_off, !fprf);
      for (const u32 seed : seeds)
      {
        const Program p = program(seed);
        SCOPED_TRACE(fmt::format("Interpreter {}={} {}",
                                 directed ? "PH_CPUDIFF_DIRECTED_CASE" : "PH_CPUDIFF_SEED", seed,
                                 p.label));
        reference.push_back(oracle.Run(p));
        ASSERT_FALSE(testing::Test::HasFailure()) << fmt::format("FPRF={}\n{}", fprf, Listing(p));
      }
    }
    DolphinExecutor cached(system, PowerPC::CPUCore::CachedInterpreter, fprf, enables, mode,
                          directed);
    ASSERT_STREQ(system.GetPowerPC().GetCPUName(), "Cached Interpreter");
    ASSERT_EQ(PowerPC::g_fprf_updates_off, !fprf);
#ifndef _ARCH_32
    // MMU::DBATUpdated populates direct pages on 64-bit hosts. W/I aliases can use a slow path.
    auto& memory = system.GetMemory();
    const u32 pa = wii ? MEM2_DATA : MEM1_DATA;
    const u32 ea = pa | 0x80000000;
    auto pages = reinterpret_cast<u8* const*>(memory.GetLogicalPageMappingsBase());
    ASSERT_NE(pages, nullptr);
    ASSERT_NE(pages[ea >> PowerPC::BAT_INDEX_SHIFT], nullptr);
    ASSERT_EQ(pages[ea >> PowerPC::BAT_INDEX_SHIFT] + (ea & (PowerPC::BAT_PAGE_SIZE - 1)),
              memory.GetPointerForRange(pa, 1));
#endif
    auto compare_range = [&](u32 begin, u32 end) {
      for (u32 i = begin; i < end; ++i)
      {
        const u32 seed = seeds[i];
        const Program p = program(seed);
        SCOPED_TRACE(fmt::format(
            "{}={} {}", directed ? "PH_CPUDIFF_DIRECTED_CASE" : "PH_CPUDIFF_SEED", seed, p.label));
        ASSERT_NO_FATAL_FAILURE(
            Compare(cached, p, reference[i], seed, fprf, enables, accepted, true));
      }
    };
    if (!directed)
    {
      ASSERT_NO_FATAL_FAILURE(compare_range(0, n));
      continue;
    }
    // Report the first divergence in each family, even if another family has already failed.
    for (u32 begin = 0; begin < n;)
    {
      u32 end = begin + 1;
      while (end < n && corpus[seeds[end]].group == corpus[seeds[begin]].group)
        ++end;
      compare_range(begin, end);
      begin = end;
    }
    if (!selected_case)
      CheckDirectedProfile(mode);
  }
  if (directed && !selected_case && mode == Mode::Optimized)
    CheckNativeMSR(system);
}

TEST(CpuDiff, GameCube)
{
  RunCases(false, false, true);
}

TEST(CpuDiff, GameCubeNoFPRF)
{
  RunCases(false, false, false);
}

TEST(CpuDiff, GameCubeFPExceptions)
{
  RunCases(false, true, true);
}

TEST(CpuDiff, GameCubeFPExceptionsNoFPRF)
{
  RunCases(false, true, false);
}

TEST(CpuDiff, WiiMem2)
{
  RunCases(true, false, true);
}

TEST(CpuDiff, WiiMem2NoFPRF)
{
  RunCases(true, false, false);
}

TEST(CpuDiff, NoPIC)
{
  RunCases(false, false, true, Mode::NoPIC);
}

TEST(CpuDiff, Optimized)
{
  RunCases(false, false, true, Mode::Optimized);
}

TEST(CpuDiff, DirectedOptimized)
{
  RunCases(false, false, true, Mode::Optimized, true);
}

TEST(CpuDiff, DirectedGameCube)
{
  RunCases(false, false, true, Mode::Normal, true);
}

TEST(CpuDiff, DirectedWiiNoFPRF)
{
  RunCases(true, false, false, Mode::Normal, true);
}

TEST(CpuDiff, DirectedNoPIC)
{
  RunCases(false, false, true, Mode::NoPIC, true);
}

TEST(CpuDiff, DirectedFPExceptions)
{
  RunCases(false, true, true, Mode::Normal, true);
}

TEST(CpuDiff, DirectedMMU)
{
  RunCases(false, false, true, Mode::Checked, true);
}
} // namespace
