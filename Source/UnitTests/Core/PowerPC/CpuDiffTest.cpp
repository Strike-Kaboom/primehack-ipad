// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// PrimeHack for iPad: compare guest-visible results of short Gekko programs.
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

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
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

namespace
{
constexpr u32 CODE = 0x80004000;
constexpr u32 STOP = 0x80005000;
constexpr u32 MEM1_DATA = 0x10000;
constexpr u32 MEM2_DATA = 0x10010000;
constexpr u32 MAX_BODY = 24;
constexpr u32 STEP_LIMIT = 128;
constexpr u32 PROGRAM_VECTOR = 0x700;
using Scratch = std::array<u8, 1024>;

struct State
{
  std::array<u32, 32> gpr{};
  std::array<std::array<u64, 2>, 32> ps{};
  u32 cr{}, xer{}, fpscr{}, lr{}, ctr{}, msr{}, pc{}, srr0{}, srr1{}, exceptions{};
  bool reserve{};
  u32 reserve_address{};
  Scratch data{};
  bool operator==(const State&) const = default;
};

struct Program
{
  State initial;
  std::array<u32, 8> gqr;
  std::vector<u32> words;
  u32 physical_data;
  bool expects_trap = false;
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
    const u32 w_i = r.Below(16);
    return D(r.Pick({56, 57, 60, 61}), fp, base, w_i << 12 | (disp & 0xfff));
  }
  case 5:
  {
    const u32 wx_ix = r.Below(16);
    return X(4, r.Pick({6, 7, 38, 39}), fp, base, 3) | wx_ix << 7;
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
  for (size_t i = 0; i < p.gqr.size(); ++i)
  {
    const u32 load = types[i % std::size(types)];
    const u32 store = types[(i + seed % std::size(types)) % std::size(types)];
    p.gqr[i] = (r.Word() & 0x3f003f00) | load << 16 | store;
  }

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

void InstallBATs(Core::System& system)
{
  // CBoot::SetupBAT is private. These are its OS mappings, including the W/I mirror.
  auto& s = system.GetPPCState();
  s.spr[SPR_IBAT0U] = s.spr[SPR_DBAT0U] = 0x80001fff;
  s.spr[SPR_IBAT0L] = s.spr[SPR_DBAT0L] = 0x00000002;
  s.spr[SPR_DBAT1U] = 0xc0001fff;
  s.spr[SPR_DBAT1L] = 0x0000002a;
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
  for (size_t i = 0; i < program.gqr.size(); ++i)
    GQR(cpu, i) = program.gqr[i];
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
  cpu.fpscr.Hex = initial.fpscr;
  PowerPC::RoundingModeUpdated(cpu);

  auto& memory = system.GetMemory();
  memory.CopyToEmu(program.physical_data, initial.data.data(), initial.data.size());
  for (size_t i = 0; i < program.words.size(); ++i)
    memory.Write_U32(program.words[i], (CODE & 0x1fffffff) + 4 * i);
  // HID0.ICE stays at its reset value (fetch directly from RAM). Evict compiled blocks for the
  // whole previous program as well, including blocks entered in its middle after a taken branch.
  system.GetJitInterface().InvalidateICache(CODE, 4 * (MAX_BODY + 1), true);
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
  s.exceptions = cpu.Exceptions;
  s.reserve = cpu.reserve;
  s.reserve_address = cpu.reserve ? cpu.reserve_address : 0;
  system.GetMemory().CopyFromEmu(s.data.data(), p.physical_data, s.data.size());
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
  DolphinExecutor(Core::System& system, PowerPC::CPUCore core, bool fprf, bool enables)
      : m_system(system)
  {
    const bool cached = core == PowerPC::CPUCore::CachedInterpreter;
    Config::SetCurrent(Config::MAIN_CPU_CORE, core);
    Config::SetCurrent(Config::MAIN_FPRF, fprf);
    Config::SetCurrent(Config::MAIN_FLOAT_EXCEPTIONS, enables);
    Config::SetCurrent(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS, enables);
    Config::SetCurrent(Config::MAIN_CIR_PS_NEON, cached);
    Config::SetCurrent(Config::MAIN_CIR_PSQ_FASTPATH, cached);
    // PSQ is a shared handler with a process-global switch. The plain Interpreter doesn't reset it.
    PowerPC::SetPsqFastpathEnabled(false);
    system.GetCoreTiming().Init();
    system.GetPowerPC().Init(core);
    if (!cached)
      PowerPC::g_fprf_updates_off = !fprf;
    InstallBATs(system);
  }

  ~DolphinExecutor() override
  {
    m_system.GetPowerPC().Shutdown();
    m_system.GetCoreTiming().Shutdown();
    PowerPC::g_fprf_updates_off = false;
    PowerPC::SetPsqFastpathEnabled(false);
  }

  std::string_view Name() const override { return m_system.GetPowerPC().GetCPUName(); }

  State Run(const Program& program) override
  {
    Install(m_system, program);
    const u32 end = CODE + static_cast<u32>(4 * program.words.size());
    auto& cpu = m_system.GetPPCState();
    for (u32 step = 0; step < STEP_LIMIT && cpu.pc >= CODE && cpu.pc < end && !(cpu.pc & 3);
         ++step)
    {
      // Interpreter steps one instruction. CachedInterpreter compiles on a miss and dispatches
      // an entire block on a hit. Inspect both only at an architectural exit.
      m_system.GetPowerPC().SingleStep();
    }
    return Capture(m_system, program);
  }

private:
  Core::System& m_system;
};

std::string Listing(const Program& p)
{
  std::string text;
  for (size_t i = 0; i < p.words.size(); ++i)
  {
    const u32 pc = CODE + static_cast<u32>(4 * i);
    text += fmt::format("{:08x}  {:08x}  {}\n", pc, p.words[i],
                        Common::GekkoDisassembler::Disassemble(p.words[i], pc));
  }
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

bool ValidExit(const Program& p, const State& s, bool enables)
{
  if (s.exceptions != 0)
    return false;
  if (s.pc == PROGRAM_VECTOR)
    return enables && (s.srr1 & (1 << 20)); // SRR1 program exception cause = floating point
  return !p.expects_trap && s.pc == STOP;
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
             bool enables)
{
  const State got = candidate.Run(p);
  if (!ValidExit(p, ref, enables) || !ValidExit(p, got, enables) || got != ref ||
      testing::Test::HasFailure())
  {
    FAIL() << fmt::format("PH_CPUDIFF_SEED={} FPRF={} FP enables={}\n"
                          "Exit PCs Interpreter={:08x} {}={:08x}, "
                          "stop={:08x}, FP trap required={}, dispatch cap={}\n"
                          "SRR1={:08x}/{:08x}, pending exceptions={:08x}/{:08x}\n{}{}",
                          seed, fprf, enables, ref.pc, candidate.Name(), got.pc, STOP, p.expects_trap,
                          STEP_LIMIT, ref.srr1, got.srr1, ref.exceptions, got.exceptions, Listing(p),
                          Differences(ref, got, p.physical_data, candidate.Name()));
  }
}

void RunCases(bool wii, bool enables, bool fprf)
{
  u32 first = 0;
  u32 count = 20000;
  if (const char* replay = std::getenv("PH_CPUDIFF_SEED"); replay && *replay)
  {
    ASSERT_TRUE(ParseU32(replay, first)) << "Invalid PH_CPUDIFF_SEED: " << replay;
    count = 1;
  }
  else if (const char* cases = std::getenv("PH_CPUDIFF_CASES"))
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
  Config::SetCurrent(Config::MAIN_MMU, false);
  Config::SetCurrent(Config::MAIN_CPU_THREAD, false);
  Config::SetCurrent(Config::MAIN_ACCURATE_CPU_CACHE, false);
  Config::SetCurrent(Config::MAIN_PAUSE_ON_PANIC, false);
  Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false); // CIR uses Memory's BAT page pointers.
  Config::SetCurrent(Config::MAIN_JIT_FOLLOW_BRANCH, false); // observe the terminal b before STOP
  Config::SetCurrent(Config::MAIN_CIR_LONG_BLOCKS, true);
  Config::SetCurrent(Config::MAIN_CIR_PIC_LOADSTORE, true);
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
  auto& memory = system.GetMemory();
  memory.Init();
  Common::ScopeGuard memory_guard([&] {
    memory.Shutdown();
    system.SetIsWii(old_wii);
  });
  ASSERT_NE(memory.GetRAM(), nullptr);
  if (wii)
    ASSERT_NE(memory.GetEXRAM(), nullptr);

  // Batching amortizes real core initialization while bounding the oracle's memory usage.
  // Each batch/engine gets a fresh CoreTiming event queue and an empty translation cache.
  for (u64 offset = 0; offset < count; offset += 2048)
  {
    const u32 n = std::min<u32>(2048, count - static_cast<u32>(offset));
    const u32 start = first + static_cast<u32>(offset);
    std::vector<State> reference;
    reference.reserve(n);
    {
      DolphinExecutor oracle(system, PowerPC::CPUCore::Interpreter, fprf, enables);
      ASSERT_STREQ(system.GetPowerPC().GetCPUName(), system.GetInterpreter().GetName());
      ASSERT_EQ(PowerPC::g_fprf_updates_off, !fprf);
      for (u32 i = 0; i < n; ++i)
      {
        const Program p = Generate(start + i, wii, enables);
        reference.push_back(oracle.Run(p));
        ASSERT_FALSE(testing::Test::HasFailure())
            << fmt::format("Interpreter PH_CPUDIFF_SEED={} FPRF={}\n{}", start + i, fprf,
                           Listing(p));
      }
    }
    DolphinExecutor cached(system, PowerPC::CPUCore::CachedInterpreter, fprf, enables);
    ASSERT_STREQ(system.GetPowerPC().GetCPUName(), "Cached Interpreter");
    ASSERT_EQ(PowerPC::g_fprf_updates_off, !fprf);
#ifndef _ARCH_32
    // MMU::DBATUpdated populates direct pages on 64-bit hosts. W/I aliases can use a slow path.
    const u32 pa = wii ? MEM2_DATA : MEM1_DATA;
    const u32 ea = pa | 0x80000000;
    auto pages = reinterpret_cast<u8* const*>(memory.GetLogicalPageMappingsBase());
    ASSERT_NE(pages, nullptr);
    ASSERT_NE(pages[ea >> PowerPC::BAT_INDEX_SHIFT], nullptr);
    ASSERT_EQ(pages[ea >> PowerPC::BAT_INDEX_SHIFT] + (ea & (PowerPC::BAT_PAGE_SIZE - 1)),
              memory.GetPointerForRange(pa, 1));
#endif
    for (u32 i = 0; i < n; ++i)
    {
      const u32 seed = start + i;
      const Program p = Generate(seed, wii, enables);
      ASSERT_NO_FATAL_FAILURE(Compare(cached, p, reference[i], seed, fprf, enables));
    }
  }
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
}  // namespace
