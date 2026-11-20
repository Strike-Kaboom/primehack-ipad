// Copyright 2008-2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "AotAbi.h"
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define PH_COLD __declspec(noinline)
#else
#define PH_COLD [[gnu::cold, gnu::noinline]]
#endif

namespace ph
{
using u8 = PHU8;
using u16 = PHU16;
using u32 = PHU32;
using u64 = PHU64;
using s32 = PHS32;
using s64 = long long;

template <class To, class From>
inline To Bits(From value)
{
  return __builtin_bit_cast(To, value);
}

// ConditionRegister::PPCToInternal/GetField, Interpreter::Helper_IntCompare.
inline u64 EncodeCR(u32 v)
{
  return 0x100000000ull | (u64(v & 1) << 59) | u64(!(v & 2)) |
         (u64(!(v & 4)) << 63) | (u64(!!(v & 8)) << 62);
}
inline u32 DecodeCR(u64 v)
{
  return ((v >> 59) & 9) | (u32(!u32(v)) << 1) | (u32(s64(v) > 0) << 2);
}
// Interpreter_Integer.cpp, Helper_UpdateCR0. Preserve its raw internal CR representation.
inline u64 RecordCR(u32 value, u8 so_ov)
{
  u64 bits = u64(s64(s32(value)));
  if (!value) bits |= 1ull << 63;
  return (bits & ~(1ull << 59)) | (u64(so_ov >> 1) << 59);
}
template <class T>
inline u64 Compare(T a, T b, u8 so_ov)
{
  return EncodeCR((a < b ? 8 : a > b ? 4 : 2) | u32(bool(so_ov >> 1)));
}
// PowerPCState::SetXER_OV. SO sticks even if a later operation clears OV.
inline void SetOverflow(u8& so_ov, bool ov)
{
  so_ov = (so_ov & 0xfe) | (ov ? 3 : 0);
}

// PrimeHack for iPad: big-endian guest transactions on little-endian hosts.
template <class T>
inline T Swap(T v)
{
  if constexpr (sizeof(T) == 8) return __builtin_bswap64(v);
  if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
  if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
  return v;
}
// PrimeHack for iPad: check the full transaction before any guest-visible effect.
PH_COLD inline const u8* PhysicalWIPage(const PHAotHost& h, u32 bat, u32 ea, u32 size)
{
  const u32 phys = (bat & ~7u) | (ea & 0x1ffff);
  const u32 limit = phys < 0x10000000 ? h.ram_size : h.exram_size;
  const u32 offset = phys < 0x10000000 ? phys : phys - 0x10000000;
  if ((bat & 5) != 5 || !h.physical_pages || offset >= limit || size > limit - offset)
    return nullptr;
  return static_cast<const u8*>(h.physical_pages[phys >> 17]);
}

inline u8* Page(const PHAotHost& h, u32 msr, u32 ea, u32 size, u32 align, bool store)
{
  if (!(msr & 16) || !h.logical_pages || !h.dbat || (ea & align) ||
      (ea & 0x1ffff) > 0x20000 - size)
    return nullptr;
  const auto index = ea >> 17;
  if (store && (size < 4 || (ea & 3)) && (h.dbat[index] & 4))
    return nullptr;
  auto* page = static_cast<const u8*>(h.logical_pages[index]);
  if (!page) [[unlikely]]
    page = PhysicalWIPage(h, h.dbat[index], ea, size);
  return page ? const_cast<u8*>(page) + (ea & 0x1ffff) : nullptr;
}

template <class T> inline T Read(const u8* PH_RESTRICT p)
{
  T value;
  __builtin_memcpy(&value, p, sizeof(value));
  return Swap(value);
}
template <class T> inline void Write(u8* PH_RESTRICT p, T value)
{
  value = Swap(value);
  __builtin_memcpy(p, &value, sizeof(value));
}

inline bool GatherAddress(const PHAotHost& h, u32 msr, u32 ea, u32 size)
{
  if (!(msr & 16) || !h.dbat || (ea & (size >= 4 ? 3 : size - 1)) ||
      (ea & 0xfff) > 0x1000 - size)
    return false;
  const u32 bat = h.dbat[ea >> 17];
  return (bat & 1) && (((bat & ~7u) | (ea & 0x1ffff)) & 0xfffff000) == 0x0c008000;
}

template <class Burst>
PH_COLD inline void FlushGather(Burst& burst)
{
  burst();
}

template <class T, class Burst> inline void GatherWord(PHState& s, T value, Burst& burst)
{
  Write<T>(s.gather_pipe_ptr, value);
  s.gather_pipe_ptr += sizeof(T);
  if (s.gather_pipe_ptr - s.gather_pipe_base_ptr >= 32) FlushGather(burst);
}

template <class T, class Burst> inline void Gather(PHState& s, T value, Burst& burst)
{
  if constexpr (sizeof(T) == 8)
  {
    GatherWord(s, u32(value >> 32), burst);
    GatherWord(s, u32(value), burst);
  }
  else
    GatherWord(s, value, burst);
}

struct FPEnv
{
  PHState& s;
  const PHAotHost& host;
  PHPair* ps;
  u64* cr;
  u32 msr, fpscr;
  bool fprf_off;
};
struct FPInst
{
  u32 FD, FA, FB, FC, Rc, CRFD;
  explicit constexpr FPInst(u32 w)
      : FD((w >> 21) & 31), FA((w >> 16) & 31), FB((w >> 11) & 31),
        FC((w >> 6) & 31), Rc(w & 1), CRFD((w >> 23) & 7) {}
};

template <int shift, int width = 1>
inline void SetField(u32& word, u32 value)
{
  constexpr u32 mask = (1u << width) - 1;
  word = (word & ~(mask << shift)) | ((value & mask) << shift);
}
inline void UpdateCR1(FPEnv& env) { env.cr[1] = EncodeCR(env.fpscr >> 28); }

// ClassifyFloat/ClassifyDouble, PowerPCState::UpdateFPRFSingle/Double.
template <class T>
inline void UpdateFPRF(FPEnv& env, T value)
{
  if (env.fprf_off)
    return;
  constexpr int frac_bits = sizeof(T) == 4 ? 23 : 52;
  constexpr u32 all_exp = sizeof(T) == 4 ? 255 : 2047;
  u64 bits;
  if constexpr (sizeof(T) == 4) bits = Bits<u32>(value);
  else bits = Bits<u64>(value);
  const bool negative = bits >> (8 * sizeof(T) - 1);
  const u32 exp = (bits >> frac_bits) & all_exp;
  const u64 frac = bits & ((1ull << frac_bits) - 1);
  u32 result;
  if (exp == all_exp) result = frac ? 0x11 : negative ? 9 : 5;
  else if (exp) result = negative ? 8 : 4;
  else if (frac) result = negative ? 0x18 : 0x14;
  else result = negative ? 0x12 : 2;
  SetField<12, 5>(env.fpscr, result);
}
constexpr u64 DOUBLE_SIGN = 1ull << 63, DOUBLE_EXP = 0x7ff0000000000000ull;
constexpr u64 DOUBLE_FRAC = 0xfffffffffffffull, DOUBLE_QBIT = 1ull << 51;
constexpr int DOUBLE_FRAC_WIDTH = 52;
inline bool IsSNAN(double d)
{
  u64 bits = Bits<u64>(d);
  return (bits & DOUBLE_EXP) == DOUBLE_EXP && (bits & DOUBLE_FRAC) && !(bits & DOUBLE_QBIT);
}
inline double MakeQuiet(double d) { return Bits<double>(Bits<u64>(d) | DOUBLE_QBIT); }
inline float FlushToZero(float f)
{
  u32 bits = Bits<u32>(f);
  return Bits<float>(bits & 0x7f800000 ? bits : bits & 0x80000000);
}
inline double FlushToZero(double f)
{
  u64 bits = Bits<u64>(f);
  return Bits<double>(bits & DOUBLE_EXP ? bits : bits & DOUBLE_SIGN);
}
inline u64 LaneBits(u64 bits) { return bits; }
inline u64 LaneBits(float f) { return Bits<u64>(double(f)); }
inline u64 LaneBits(double d) { return Bits<u64>(d); }
template <class T> inline void SetPS0(PHPair& p, T v) { p.ps0 = LaneBits(v); }
template <class T> inline void SetBoth(PHPair& p, T a, T b) { p = {LaneBits(a), LaneBits(b)}; }
template <class T> inline void Fill(PHPair& p, T v) { SetBoth(p, v, v); }

enum FPSCRExceptionFlag : u32
{
  FPSCR_FX = 1U << (31 - 0),
  FPSCR_FEX = 1U << (31 - 1),
  FPSCR_VX = 1U << (31 - 2),
  FPSCR_OX = 1U << (31 - 3),
  FPSCR_UX = 1U << (31 - 4),
  FPSCR_ZX = 1U << (31 - 5),
  FPSCR_XX = 1U << (31 - 6),
  FPSCR_VXSNAN = 1U << (31 - 7),
  FPSCR_VXISI = 1U << (31 - 8),
  FPSCR_VXIDI = 1U << (31 - 9),
  FPSCR_VXZDZ = 1U << (31 - 10),
  FPSCR_VXIMZ = 1U << (31 - 11),
  FPSCR_VXVC = 1U << (31 - 12),
  FPSCR_VXSOFT = 1U << (31 - 21),
  FPSCR_VXSQRT = 1U << (31 - 22),
  FPSCR_VXCVI = 1U << (31 - 23),
  FPSCR_VE = 1U << (31 - 24),
  FPSCR_OE = 1U << (31 - 25),
  FPSCR_UE = 1U << (31 - 26),
  FPSCR_ZE = 1U << (31 - 27),
  FPSCR_XE = 1U << (31 - 28),
  FPSCR_VX_ANY = 0x01f80700u,
  FPSCR_ANY_E = 0xf8
};
constexpr u32 FPCC_MASK = 0xf000;
struct BaseAndDec { int m_base, m_dec; };
inline constexpr BaseAndDec frsqrte_expected[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

// FloatUtils.cpp, ApproximateReciprocalSquareRoot.
inline double ApproximateReciprocalSquareRoot(double val)
{
  s64 integral = Bits<s64>(val);
  s64 mantissa = integral & ((1LL << 52) - 1);
  const s64 sign = integral & (1ULL << 63);
  s64 exponent = integral & (0x7FFLL << 52);

  // Special case 0
  if (mantissa == 0 && exponent == 0)
  {
    return sign ? -__builtin_huge_val() :
                  __builtin_huge_val();
  }

  // Special case NaN-ish numbers
  if (exponent == (0x7FFLL << 52))
  {
    if (mantissa == 0)
    {
      if (sign)
        return __builtin_nan("");

      return 0.0;
    }

    return MakeQuiet(val);
  }

  // Negative numbers return NaN
  if (sign)
    return __builtin_nan("");

  if (!exponent)
  {
    // "Normalize" denormal values
    do
    {
      exponent -= 1LL << 52;
      mantissa <<= 1;
    } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }

  const s64 exponent_lsb = exponent & (1LL << 52);
  exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
  integral = sign | exponent;

  const int i = static_cast<int>((exponent_lsb | mantissa) >> 37);
  const auto& entry = frsqrte_expected[i / 2048];
  integral |= static_cast<s64>(entry.m_base + entry.m_dec * (i % 2048)) << 26;

  return Bits<double>(integral);
}

inline constexpr BaseAndDec fres_expected[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};

// Used by fres and ps_res.
// FloatUtils.cpp, ApproximateReciprocal.
inline double ApproximateReciprocal(double val)
{
  s64 integral = Bits<s64>(val);
  const s64 mantissa = integral & ((1LL << 52) - 1);
  const s64 sign = integral & (1ULL << 63);
  s64 exponent = integral & (0x7FFLL << 52);

  // Special case 0
  if (mantissa == 0 && exponent == 0)
    return __builtin_copysign(__builtin_huge_val(), val);

  // Special case NaN-ish numbers
  if (exponent == (0x7FFLL << 52))
  {
    if (mantissa == 0)
      return __builtin_copysign(0.0, val);
    return MakeQuiet(val);
  }

  // Special case small inputs
  if (exponent < (895LL << 52))
    return __builtin_copysign(__FLT_MAX__, val);

  // Special case large inputs
  if (exponent >= (1149LL << 52))
    return __builtin_copysign(0.0, val);

  exponent = (0x7FDLL << 52) - exponent;

  const int i = static_cast<int>(mantissa >> 37);
  const auto& entry = fres_expected[i / 1024];
  integral = sign | exponent;
  integral |= static_cast<s64>(entry.m_base - (entry.m_dec * (i % 1024) + 1) / 2) << 29;

  return Bits<double>(integral);
}

constexpr double PPC_NAN = __builtin_nan("");

// the 4 less-significand bits in FPSCR[FPRF]
enum class FPCC
{
  FL = 8,  // <
  FG = 4,  // >
  FE = 2,  // =
  FU = 1,  // ?
};

// Interpreter_FPUtils.h, SetFPException/UpdateFPExceptionSummary/CheckFPExceptions.
PH_COLD inline void SetFPException(FPEnv& env, u32 mask)
{
  auto& f = env.fpscr;
  if ((f & mask) != mask) f |= FPSCR_FX;
  f |= mask;
  SetField<29>(f, (f & FPSCR_VX_ANY) != 0);
  SetField<30>(f, ((f >> 22) & (f & FPSCR_ANY_E)) != 0);
  if ((f & FPSCR_FEX) && (env.msr & 0x900))
  {
    env.s.Exceptions |= 0x80;
    env.s.spr[27] = 1u << 20;  // GenerateProgramException(FloatingPoint)
  }
}

// Interpreter_FPUtils.h, ForceSingle.
inline float ForceSingle(const FPEnv& env, double value)
{
  if ((env.fpscr & 4))
  {
    // Emulate a rounding quirk. If the conversion result before rounding is a subnormal single,
    // it's always flushed to zero, even if rounding would have caused it to become normal.

    constexpr u64 smallest_normal_single = 0x3810000000000000;
    const u64 value_without_sign =
        Bits<u64>(value) & (DOUBLE_EXP | DOUBLE_FRAC);

    if (value_without_sign < smallest_normal_single)
    {
      const u64 flushed_double = Bits<u64>(value) & DOUBLE_SIGN;
      const u32 flushed_single = static_cast<u32>(flushed_double >> 32);
      return Bits<float>(flushed_single);
    }
  }

  // Emulate standard conversion to single precision.

  float x = static_cast<float>(value);
  if (!env.host.hardware_ftz && (env.fpscr & 4))
  {
    x = FlushToZero(x);
  }
  return x;
}

// Interpreter_FPUtils.h, ForceDouble.
inline double ForceDouble(const FPEnv& env, double d)
{
  if (!env.host.hardware_ftz && (env.fpscr & 4))
  {
    d = FlushToZero(d);
  }
  return d;
}

// Interpreter_FPUtils.h, Force25Bit.
inline double Force25Bit(double d)
{
  u64 integral = Bits<u64>(d);

  u64 exponent = integral & DOUBLE_EXP;
  u64 fraction = integral & DOUBLE_FRAC;

  if (exponent == 0 && fraction != 0)
  {
    // Subnormals get "normalized" before they're rounded
    // In the end, this practically just means that the rounding is
    // at a different bit

    s64 keep_mask = 0xFFFFFFFFF8000000LL;
    u64 round = 0x8000000;

    // Shift the mask and rounding bit to the right until
    // the fraction is "normal"
    // That is to say shifting it until the MSB of the fraction
    // would escape into the exponent
    u32 shift = __builtin_clzll(fraction) - (63 - DOUBLE_FRAC_WIDTH);
    keep_mask >>= shift;
    round >>= shift;

    // Round using these shifted values
    integral = (integral & keep_mask) + (integral & round);
  }
  else
  {
    integral = (integral & 0xFFFFFFFFF8000000ULL) + (integral & 0x8000000);
  }

  return Bits<double>(integral);
}

// these functions allow globally modify operations behaviour
// also, these may be used to set flags like FR, FI, OX, UX

struct FPResult
{
  bool HasNoInvalidExceptions() const { return (exception & FPSCR_VX_ANY) == 0; }

  void SetException(FPEnv& env, FPSCRExceptionFlag flag)
  {
    exception = flag;
    SetFPException(env, flag);
  }

  double value = 0.0;
  FPSCRExceptionFlag exception{};
};

// Interpreter_FPUtils.h, NI_mul.
PH_COLD inline FPResult NI_mul_nan(FPEnv& env, double a, double b)
{
  FPResult result;
  if (IsSNAN(a) || IsSNAN(b))
    result.SetException(env, FPSCR_VXSNAN);
  env.fpscr &= ~(3u << 17);
  if (__builtin_isnan(a))
    result.value = MakeQuiet(a);
  else if (__builtin_isnan(b))
    result.value = MakeQuiet(b);
  else
  {
    result.value = PPC_NAN;
    result.SetException(env, FPSCR_VXIMZ);
  }
  return result;
}

inline FPResult NI_mul(FPEnv& env, double a, double b)
{
  FPResult result{a * b};
  if (__builtin_isnan(result.value)) [[unlikely]]
    return NI_mul_nan(env, a, b);
  return result;
}

// Interpreter_FPUtils.h, NI_div.
PH_COLD inline FPResult NI_div_exception(FPEnv& env, double a, double b, FPResult result)
{
  if (__builtin_isinf(result.value))
  {
    result.SetException(env, FPSCR_ZX);
    return result;
  }
  if (IsSNAN(a) || IsSNAN(b))
    result.SetException(env, FPSCR_VXSNAN);
  env.fpscr &= ~(3u << 17);
  if (__builtin_isnan(a))
    result.value = MakeQuiet(a);
  else if (__builtin_isnan(b))
    result.value = MakeQuiet(b);
  else
  {
    if (b == 0.0)
      result.SetException(env, FPSCR_VXZDZ);
    else if (__builtin_isinf(a) && __builtin_isinf(b))
      result.SetException(env, FPSCR_VXIDI);
    result.value = PPC_NAN;
  }
  return result;
}

inline FPResult NI_div(FPEnv& env, double a, double b)
{
  FPResult result{a / b};
  if (__builtin_isnan(result.value) || (__builtin_isinf(result.value) && b == 0.0)) [[unlikely]]
    return NI_div_exception(env, a, b, result);
  return result;
}

// PrimeHack for iPad (no-JIT speed): the NaN tail shared by NI_add and NI_sub (identical text in
// both), moved out of line and marked cold so the common one-instruction case inlines into the
// interpreter handlers. Before, clang emitted NI_add/NI_sub as ~400-byte out-of-line functions, so
// every fadds/fsubs paid a call plus an FPResult round trip through integer registers.
// Interpreter_FPUtils.h, NI_add_sub_nan.
PH_COLD inline FPResult NI_add_sub_nan(FPEnv& env, double a, double b)
{
  FPResult result;

  if (IsSNAN(a) || IsSNAN(b))
    result.SetException(env, FPSCR_VXSNAN);

  env.fpscr &= ~(3u << 17);

  if (__builtin_isnan(a))
  {
    result.value = MakeQuiet(a);
    return result;
  }
  if (__builtin_isnan(b))
  {
    result.value = MakeQuiet(b);
    return result;
  }

  result.SetException(env, FPSCR_VXISI);
  result.value = PPC_NAN;
  return result;
}

// Interpreter_FPUtils.h, NI_add.
inline FPResult NI_add(FPEnv& env, double a, double b)
{
  FPResult result{a + b};

  if (__builtin_isnan(result.value)) [[unlikely]]
    return NI_add_sub_nan(env, a, b);

  if (__builtin_isinf(a) || __builtin_isinf(b)) [[unlikely]]
    env.fpscr &= ~(3u << 17);

  return result;
}

// Interpreter_FPUtils.h, NI_sub.
inline FPResult NI_sub(FPEnv& env, double a, double b)
{
  FPResult result{a - b};

  if (__builtin_isnan(result.value)) [[unlikely]]
    return NI_add_sub_nan(env, a, b);

  if (__builtin_isinf(a) || __builtin_isinf(b)) [[unlikely]]
    env.fpscr &= ~(3u << 17);

  return result;
}

// PrimeHack for iPad: NaN tail of NI_madd_msub, moved out of line unchanged (a, c, b order kept).
// Interpreter_FPUtils.h, NI_madd_msub_nan.
PH_COLD inline FPResult NI_madd_msub_nan(FPEnv& env, double a, double c, double b)
{
  FPResult result;

  if (IsSNAN(a) || IsSNAN(b) || IsSNAN(c))
    result.SetException(env, FPSCR_VXSNAN);

  env.fpscr &= ~(3u << 17);

  if (__builtin_isnan(a))
  {
    result.value = MakeQuiet(a);
    return result;
  }
  if (__builtin_isnan(b))
  {
    result.value = MakeQuiet(b);  // !
    return result;
  }
  if (__builtin_isnan(c))
  {
    result.value = MakeQuiet(c);
    return result;
  }

  result.SetException(env, __builtin_isnan(a * c) ? FPSCR_VXIMZ : FPSCR_VXISI);
  result.value = PPC_NAN;
  return result;
}

// The bulk work for fm(add/sub)(s)x operations
template <bool sub, bool single>
// Interpreter_FPUtils.h, NI_madd_msub.
inline FPResult NI_madd_msub(FPEnv& env, double a, double c, double b)
{
  // Correct single-rounding ties with the same error-free FMA transform as Dolphin.
  FPResult result;

  // In double precision, just doing the normal operation will be exact with no issues.
  if (!single)
  {
    result.value = __builtin_fma(a, c, sub ? -b : b);
  }
  else
  {
    // For single precision inputs, we never actually cast to a float -- we instead compute the
    // result using a 64-bit FMA, and if the bits end up being an even tie when converting to
    // a float, we approximate (for single-precision-only inputs this will be exact) the
    // amount rounded by the FMA, and use that to manually fix which direction we round!
    // We of course still properly round `c` first, though.
    const double c_round = Force25Bit(c);

    // First, we compute the 64-bit FMA forwards
    const double b_sign = sub ? -b : b;
    result.value = __builtin_fma(a, c_round, b_sign);

    // We then check if we're currently tying in rounding direction
    const u64 result_bits = Bits<u64>(result.value);

    // The mask of the `d` bits as shown in the above comments
    const u64 D_MASK = 0x000000001fffffff;
    // The mask of `d` which would force a tie to even, which is the only case where there
    // can be potentially be differences compared to just casting to an f32 directly.
    const u64 EVEN_TIE = 0x0000000010000000;

    // Because we check this entire mask which includes a 1 bit, we can be sure that
    // if this result passes, the input is not an infinity that would become a NaN.
    // If we had only checked for a subset of these bits (e.g. only checking if the last
    // one was 0), we would have needed to also check if the exponent was all ones.
    if ((result_bits & D_MASK) == EVEN_TIE)
    {
      // Because we have a tie, we now compute any error in the FMA calculation
      // via an error-free transformation (Ole Møller's 2Sum algorithm)
      // s  := a  + b
      // a' := s  - b
      // b' := s  - a'
      // da := a  - a'
      // db := b  - b'
      // t  := da + db
      // But for these calculations, we assume "a" := a * c_round, allowing the usage of FMA,
      // both being shorter and allowing for likely necessary increased precision!
      // We also switch up the signs a bit so we don't introduce an instruction simply to
      // negate one of the operands of an FMA
      const double a_prime = b_sign - result.value;
      const double b_prime = result.value + a_prime;
      const double delta_a = __builtin_fma(a, c_round, a_prime);
      const double delta_b = b_sign - b_prime;
      const double error = delta_a + delta_b;

      // `error` will properly match the direction for rounding *even for 64-bit inputs*.
      // Thoroughly proving that this works for even all normal values isn't entirely trivial,
      // nor are the exact details really important, but the basic logic is:
      // result.value = roundf64(a * c_round + b_sign) = a * c_round + b_sign - e0
      // a_prime = roundf64(b_sign - a * c_round - b_sign + e0)
      //         = -a * c_round + e0 - e1
      // b_prime = roundf64(a * c_round + b_sign - e0 - a * c_round + e0 - e1)
      //         = b_sign - e1 - e2
      // delta_a = roundf64(a * c_round - a * c_round + e0 - e1)
      //         = e0 - e1 - e3
      // delta_b = roundf64(b_sign - b_sign + e1 + e2)
      //         = e1 + e2 - e4
      // error   = roundf64(delta_a + delta_b)
      //         = roundf64(e0 + e2 - e3 - e4)
      // Then showing that e2 - e3 - e4 is tiny enough to not change the sign of
      // e0 (the true error value, as `error` can't capture all of the possible precision),
      // including that if the true e0 = 0 then e1 = e2 = e3 = e4 = error = 0.

      // This "error" value represents the number such that `result.value - error == exact_result`.
      if (error != 0.0)
      {
        // Because the error is nonzero here, we actually do need to round a specific direction
        // and don't want to just tie to even!

        // Note that it should never be possible for the error to be NaN if the result isn't either
        // infinite or NaN itself. It would require:
        // da == inf, db == -inf
        // Which expanded out is:
        // a - ((a + b) - b) == inf, b - ((a + b) - ((a + b) - b)) == -inf, where
        // a + b isn't infinite. This means (a + b) - b must be infinite on the left,
        // but this will end up giving the right hand side the same sign of infinity.
        // All this to say we don't check for `if (!__builtin_isnan(error))` for the `else` statement.
        // Also note that we do not cast to a float here,
        // as individual instructions using this function will on their own afterwards.

        if ((error > 0.0) == (result.value > 0.0))
          result.value = Bits<double>(result_bits + 1);  // Tie is too small, round up.
        else
          result.value = Bits<double>(result_bits - 1);  // Tie is too large, round down.
      }
    }
  }

  if (__builtin_isnan(result.value)) [[unlikely]]
    return NI_madd_msub_nan(env, a, c, b);

  if (__builtin_isinf(a) || __builtin_isinf(b) || __builtin_isinf(c)) [[unlikely]]
    env.fpscr &= ~(3u << 17);

  return result;
}

template <bool single>
// Interpreter_FPUtils.h, NI_madd.
inline FPResult NI_madd(FPEnv& env, double a, double c, double b)
{
  return NI_madd_msub<false, single>(env, a, c, b);
}

template <bool single>
// Interpreter_FPUtils.h, NI_msub.
inline FPResult NI_msub(FPEnv& env, double a, double c, double b)
{
  return NI_madd_msub<true, single>(env, a, c, b);
}

// used by stfsXX instructions and ps_rsqrte
// Interpreter_FPUtils.h, ConvertToSingle.
inline u32 ConvertToSingle(u64 x)
{
  const u32 exp = u32((x >> 52) & 0x7ff);

  if (exp > 896 || (x & ~DOUBLE_SIGN) == 0)
  {
    return u32(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  }
  else if (exp >= 874)
  {
    u32 t = u32(0x80000000 | ((x & DOUBLE_FRAC) >> 21));
    t = t >> (905 - exp);
    t |= u32((x >> 32) & 0x80000000);
    return t;
  }
  else
  {
    // This is said to be undefined.
    // The code is based on hardware tests.
    return u32(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  }
}

// used by psq_stXX operations.
// Interpreter_FPUtils.h, ConvertToSingleFTZ.
inline u32 ConvertToSingleFTZ(u64 x)
{
  const u32 exp = u32((x >> 52) & 0x7ff);

  if (exp > 896 || (x & ~DOUBLE_SIGN) == 0)
  {
    return u32(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  }
  else
  {
    return u32((x >> 32) & 0x80000000);
  }
}

// Interpreter_FPUtils.h, ConvertToDouble.
inline u64 ConvertToDouble(u32 value)
{
  // This is a little-endian re-implementation of the algorithm described in
  // the PowerPC Programming Environments Manual for loading single
  // precision floating point numbers.
  // See page 566 of http://www.freescale.com/files/product/doc/MPCFPE32B.pdf

  u64 x = value;
  u64 exp = (x >> 23) & 0xff;
  u64 frac = x & 0x007fffff;

  if (exp > 0 && exp < 255)  // Normal number
  {
    u64 y = !(exp >> 7);
    u64 z = y << 61 | y << 60 | y << 59;
    return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
  }
  else if (exp == 0 && frac != 0)  // Subnormal number
  {
    exp = 1023 - 126;
    do
    {
      frac <<= 1;
      exp -= 1;
    } while ((frac & 0x00800000) == 0);

    return ((x & 0x80000000) << 32) | (exp << 52) | ((frac & 0x007fffff) << 29);
  }
  else  // QNaN, SNaN or Zero
  {
    u64 y = exp >> 7;
    u64 z = y << 61 | y << 60 | y << 59;
    return ((x & 0xc0000000) << 32) | z | ((x & 0x3fffffff) << 29);
  }
}

enum class RoundingMode
{
  Nearest = 0b00,
  TowardsZero = 0b01,
  TowardsPositiveInfinity = 0b10,
  TowardsNegativeInfinity = 0b11
};

// Interpreter_FloatingPoint.cpp, SetFI.
inline void SetFI(FPEnv& env, u32 FI)
{
  if (FI != 0)
  {
    SetFPException(env, FPSCR_XX);
  }
  SetField<17>(env.fpscr, FI);
}

// Round a number to an integer in the same direction as the CPU rounding mode,
// without setting any CPU flags or being careful about NaNs
// Interpreter_FloatingPoint.cpp, RoundToIntegerMode.
inline double RoundToIntegerMode(double number)
{
  // This value is 2^52 -- The first number in which double precision floating point
  // numbers can only store subsequent integers, and no longer any decimals
  // This keeps the sign of the unrounded value because it needs to scale it
  // upwards when added
  const double int_precision = __builtin_copysign(4503599627370496.0, number);

  // By adding this value to the original number,
  // it will be forced to decide a integer to round to
  // This rounding will be the same as the CPU rounding mode
  return (number + int_precision) - int_precision;
}

// Note that the convert to integer operation is defined
// in Appendix C.4.2 in PowerPC Microprocessor Family:
// The Programming Environments Manual for 32 and 64-bit Microprocessors
// Interpreter_FloatingPoint.cpp, ConvertToInteger.
inline void ConvertToInteger(FPEnv& env, FPInst inst,
                      RoundingMode rounding_mode)
{
  const double b = Bits<double>(env.ps[inst.FB].ps0);
  double rounded;
  u32 value;
  bool exception_occurred = false;

  // To reduce complexity, this takes in a rounding mode in a switch case,
  // rather than always judging based on the emulated CPU rounding mode
  switch (rounding_mode)
  {
  case RoundingMode::Nearest:
    // On generic platforms, the rounding should be assumed to be ties to even
    // For targeted platforms this would work for any rounding mode,
    // but it's mainly just kept in to replace roundeven,
    // due to its lack in the C++17 (and possible lack for future versions)
    rounded = RoundToIntegerMode(b);
    break;
  case RoundingMode::TowardsZero:
    rounded = __builtin_trunc(b);
    break;
  case RoundingMode::TowardsPositiveInfinity:
    rounded = __builtin_ceil(b);
    break;
  case RoundingMode::TowardsNegativeInfinity:
    rounded = __builtin_floor(b);
    break;
  default:
    __builtin_unreachable();
  }

  if (__builtin_isnan(b))
  {
    if (IsSNAN(b))
      SetFPException(env, FPSCR_VXSNAN);

    value = 0x80000000;
    SetFPException(env, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else if (rounded >= static_cast<double>(0x80000000))
  {
    // Positive large operand or +inf
    value = 0x7fffffff;
    SetFPException(env, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else if (rounded < -static_cast<double>(0x80000000))
  {
    // Negative large operand or -inf
    value = 0x80000000;
    SetFPException(env, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else
  {
    s32 signed_value = static_cast<s32>(rounded);
    value = static_cast<u32>(signed_value);
    const double di = static_cast<double>(signed_value);
    if (di == b)
    {
      env.fpscr &= ~(3u << 17);
    }
    else
    {
      // Also sets FPSCR[XX]
      SetFI(env, 1);
      SetField<18>(env.fpscr, __builtin_fabs(di) > __builtin_fabs(b));
    }
  }

  if (exception_occurred)
  {
    env.fpscr &= ~(3u << 17);
  }

  if (!exception_occurred || (env.fpscr & FPSCR_VE) == 0)
  {
    // Based on HW tests
    // FPRF is not affected
    u64 result = 0xfff8000000000000ull | value;
    if (value == 0 && __builtin_signbit(b))
      result |= 0x100000000ull;

    SetPS0(env.ps[inst.FD], result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, Helper_FloatCompareOrdered.
inline void Helper_FloatCompareOrdered(FPEnv& env,
                                             FPInst inst, double fa, double fb)
{
  FPCC compare_result;

  if (__builtin_isnan(fa) || __builtin_isnan(fb))
  {
    compare_result = FPCC::FU;
    if (IsSNAN(fa) || IsSNAN(fb))
    {
      SetFPException(env, FPSCR_VXSNAN);
      if ((env.fpscr & FPSCR_VE) == 0)
      {
        SetFPException(env, FPSCR_VXVC);
      }
    }
    else  // QNaN
    {
      SetFPException(env, FPSCR_VXVC);
    }
  }
  else if (fa < fb)
  {
    compare_result = FPCC::FL;
  }
  else if (fa > fb)
  {
    compare_result = FPCC::FG;
  }
  else  // Equals
  {
    compare_result = FPCC::FE;
  }

  const u32 compare_value = static_cast<u32>(compare_result);

  // Clear and set the FPCC bits accordingly.
  SetField<12, 5>(env.fpscr, (((env.fpscr >> 12) & 31) & ~FPCC_MASK) | compare_value);

  env.cr[inst.CRFD] = EncodeCR(compare_value);
}

// Interpreter_FloatingPoint.cpp, Helper_FloatCompareUnordered.
inline void Helper_FloatCompareUnordered(FPEnv& env,
                                               FPInst inst, double fa, double fb)
{
  FPCC compare_result;

  if (__builtin_isnan(fa) || __builtin_isnan(fb))
  {
    compare_result = FPCC::FU;

    if (IsSNAN(fa) || IsSNAN(fb))
    {
      SetFPException(env, FPSCR_VXSNAN);
    }
  }
  else if (fa < fb)
  {
    compare_result = FPCC::FL;
  }
  else if (fa > fb)
  {
    compare_result = FPCC::FG;
  }
  else  // Equals
  {
    compare_result = FPCC::FE;
  }

  const u32 compare_value = static_cast<u32>(compare_result);

  // Clear and set the FPCC bits accordingly.
  SetField<12, 5>(env.fpscr, (((env.fpscr >> 12) & 31) & ~FPCC_MASK) | compare_value);

  env.cr[inst.CRFD] = EncodeCR(compare_value);
}

// Interpreter_FloatingPoint.cpp, fcmpo.
inline void fcmpo(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareOrdered(env, inst, Bits<double>(a.ps0), Bits<double>(b.ps0));
}

// Interpreter_FloatingPoint.cpp, fcmpu.
inline void fcmpu(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareUnordered(env, inst, Bits<double>(a.ps0), Bits<double>(b.ps0));
}

// Interpreter_FloatingPoint.cpp, fctiwx.
inline void fctiwx(FPEnv& env, FPInst inst)
{
  ConvertToInteger(env, inst, static_cast<RoundingMode>((env.fpscr & 3)));
}

// Interpreter_FloatingPoint.cpp, fctiwzx.
inline void fctiwzx(FPEnv& env, FPInst inst)
{
  ConvertToInteger(env, inst, RoundingMode::TowardsZero);
}

// Interpreter_FloatingPoint.cpp, fmrx.
inline void fmrx(FPEnv& env, FPInst inst)
{
  SetPS0(env.ps[inst.FD], env.ps[inst.FB].ps0);

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fabsx.
inline void fabsx(FPEnv& env, FPInst inst)
{
  SetPS0(env.ps[inst.FD], __builtin_fabs(Bits<double>(env.ps[inst.FB].ps0)));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnabsx.
inline void fnabsx(FPEnv& env, FPInst inst)
{
  SetPS0(env.ps[inst.FD], env.ps[inst.FB].ps0 | (1ull << 63));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnegx.
inline void fnegx(FPEnv& env, FPInst inst)
{
  SetPS0(env.ps[inst.FD], env.ps[inst.FB].ps0 ^ (1ull << 63));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fselx.
inline void fselx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  SetPS0(env.ps[inst.FD], (Bits<double>(a.ps0) >= -0.0) ? Bits<double>(c.ps0) : Bits<double>(b.ps0));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, frspx. Both lanes receive the rounded value.
inline void frspx(FPEnv& env, FPInst inst)  // round to single
{
  const double b = Bits<double>(env.ps[inst.FB].ps0);
  const float rounded = ForceSingle(env, b);

  if (__builtin_isnan(b))
  {
    const bool is_snan = IsSNAN(b);

    if (is_snan)
      SetFPException(env, FPSCR_VXSNAN);

    if (!is_snan || (env.fpscr & FPSCR_VE) == 0)
    {
      Fill(env.ps[inst.FD], rounded);
      UpdateFPRF(env, rounded);
    }

    env.fpscr &= ~(3u << 17);
  }
  else
  {
    SetFI(env, b != rounded);
    SetField<18>(env.fpscr, __builtin_fabs(rounded) > __builtin_fabs(b));
    UpdateFPRF(env, rounded);
    Fill(env.ps[inst.FD], rounded);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fmulx.
inline void fmulx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& c = env.ps[inst.FC];

  const FPResult product = NI_mul(env, Bits<double>(a.ps0), Bits<double>(c.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(env, product.value);

    SetPS0(env.ps[inst.FD], result);
    SetField<17>(env.fpscr, 0);  // are these flags important?
    SetField<18>(env.fpscr, 0);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}
// Interpreter_FloatingPoint.cpp, fmulsx.
inline void fmulsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& c = env.ps[inst.FC];

  const double c_value = Force25Bit(Bits<double>(c.ps0));
  const FPResult product = NI_mul(env, Bits<double>(a.ps0), c_value);

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(env, product.value);

    Fill(env.ps[inst.FD], result);
    SetField<17>(env.fpscr, 0);
    SetField<18>(env.fpscr, 0);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fmaddx.
inline void fmaddx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_madd<false>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(env, product.value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fmaddsx.
inline void fmaddsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(env, product.value);

    Fill(env.ps[inst.FD], result);
    SetField<17>(env.fpscr, product.value != result);
    SetField<18>(env.fpscr, 0);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, faddx.
inline void faddx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult sum = NI_add(env, Bits<double>(a.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || sum.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(env, sum.value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}
// Interpreter_FloatingPoint.cpp, faddsx.
inline void faddsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult sum = NI_add(env, Bits<double>(a.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || sum.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(env, sum.value);
    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fdivx.
inline void fdivx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult quotient = NI_div(env, Bits<double>(a.ps0), Bits<double>(b.ps0));
  const bool not_divide_by_zero = (env.fpscr & FPSCR_ZE) == 0 || quotient.exception != FPSCR_ZX;
  const bool not_invalid = (env.fpscr & FPSCR_VE) == 0 || quotient.HasNoInvalidExceptions();

  if (not_divide_by_zero && not_invalid)
  {
    const double result = ForceDouble(env, quotient.value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  // FR,FI,OX,UX???
  if (inst.Rc)
    UpdateCR1(env);
}
// Interpreter_FloatingPoint.cpp, fdivsx.
inline void fdivsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult quotient = NI_div(env, Bits<double>(a.ps0), Bits<double>(b.ps0));
  const bool not_divide_by_zero = (env.fpscr & FPSCR_ZE) == 0 || quotient.exception != FPSCR_ZX;
  const bool not_invalid = (env.fpscr & FPSCR_VE) == 0 || quotient.HasNoInvalidExceptions();

  if (not_divide_by_zero && not_invalid)
  {
    const float result = ForceSingle(env, quotient.value);
    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Single precision only.
// Interpreter_FloatingPoint.cpp, fresx.
inline void fresx(FPEnv& env, FPInst inst)
{
  const double b = Bits<double>(env.ps[inst.FB].ps0);

  const auto compute_result = [&env, inst](double value) {
    const double result = ApproximateReciprocal(value);
    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, float(result));
  };

  if (b == 0.0)
  {
    SetFPException(env, FPSCR_ZX);
    env.fpscr &= ~(3u << 17);

    if ((env.fpscr & FPSCR_ZE) == 0)
      compute_result(b);
  }
  else if (IsSNAN(b))
  {
    SetFPException(env, FPSCR_VXSNAN);
    env.fpscr &= ~(3u << 17);

    if ((env.fpscr & FPSCR_VE) == 0)
      compute_result(b);
  }
  else
  {
    if (__builtin_isnan(b) || __builtin_isinf(b))
      env.fpscr &= ~(3u << 17);

    compute_result(b);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, frsqrtex.
inline void frsqrtex(FPEnv& env, FPInst inst)
{
  const double b = Bits<double>(env.ps[inst.FB].ps0);

  const auto compute_result = [&env, inst](double value) {
    const double result = ApproximateReciprocalSquareRoot(value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  };

  if (b < 0.0)
  {
    SetFPException(env, FPSCR_VXSQRT);
    env.fpscr &= ~(3u << 17);

    if ((env.fpscr & FPSCR_VE) == 0)
      compute_result(b);
  }
  else if (b == 0.0)
  {
    SetFPException(env, FPSCR_ZX);
    env.fpscr &= ~(3u << 17);

    if ((env.fpscr & FPSCR_ZE) == 0)
      compute_result(b);
  }
  else if (IsSNAN(b))
  {
    SetFPException(env, FPSCR_VXSNAN);
    env.fpscr &= ~(3u << 17);

    if ((env.fpscr & FPSCR_VE) == 0)
      compute_result(b);
  }
  else
  {
    if (__builtin_isnan(b) || __builtin_isinf(b))
      env.fpscr &= ~(3u << 17);

    compute_result(b);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fmsubx.
inline void fmsubx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_msub<false>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(env, product.value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fmsubsx.
inline void fmsubsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_msub<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(env, product.value);
    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnmaddx.
inline void fnmaddx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_madd<false>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const double tmp = ForceDouble(env, product.value);
    const double result = __builtin_isnan(tmp) ? tmp : -tmp;

    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnmaddsx.
inline void fnmaddsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const float tmp = ForceSingle(env, product.value);
    const float result = __builtin_isnan(tmp) ? tmp : -tmp;

    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnmsubx.
inline void fnmsubx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_msub<false>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const double tmp = ForceDouble(env, product.value);
    const double result = __builtin_isnan(tmp) ? tmp : -tmp;

    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fnmsubsx.
inline void fnmsubsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const FPResult product =
      NI_msub<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || product.HasNoInvalidExceptions())
  {
    const float tmp = ForceSingle(env, product.value);
    const float result = __builtin_isnan(tmp) ? tmp : -tmp;

    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fsubx.
inline void fsubx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult difference = NI_sub(env, Bits<double>(a.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || difference.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(env, difference.value);
    SetPS0(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_FloatingPoint.cpp, fsubsx.
inline void fsubsx(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const FPResult difference = NI_sub(env, Bits<double>(a.ps0), Bits<double>(b.ps0));

  if ((env.fpscr & FPSCR_VE) == 0 || difference.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(env, difference.value);
    Fill(env.ps[inst.FD], result);
    UpdateFPRF(env, result);
  }

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_sel.
inline void ps_sel(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  SetBoth(env.ps[inst.FD], Bits<double>(a.ps0) >= -0.0 ? Bits<double>(c.ps0) : Bits<double>(b.ps0),
                                Bits<double>(a.ps1) >= -0.0 ? Bits<double>(c.ps1) : Bits<double>(b.ps1));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_neg.
inline void ps_neg(FPEnv& env, FPInst inst)
{
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], b.ps0 ^ (1ull << 63),
                                b.ps1 ^ (1ull << 63));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_mr.
inline void ps_mr(FPEnv& env, FPInst inst)
{
  env.ps[inst.FD] = env.ps[inst.FB];

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_nabs.
inline void ps_nabs(FPEnv& env, FPInst inst)
{
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], b.ps0 | (1ull << 63),
                                b.ps1 | (1ull << 63));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_abs.
inline void ps_abs(FPEnv& env, FPInst inst)
{
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], b.ps0 & ~(1ull << 63),
                                b.ps1 & ~(1ull << 63));

  if (inst.Rc)
    UpdateCR1(env);
}

// These are just moves, double is OK.
// Interpreter_Paired.cpp, ps_merge00.
inline void ps_merge00(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], Bits<double>(a.ps0), Bits<double>(b.ps0));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_merge01.
inline void ps_merge01(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], Bits<double>(a.ps0), Bits<double>(b.ps1));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_merge10.
inline void ps_merge10(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], Bits<double>(a.ps1), Bits<double>(b.ps0));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_merge11.
inline void ps_merge11(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  SetBoth(env.ps[inst.FD], Bits<double>(a.ps1), Bits<double>(b.ps1));

  if (inst.Rc)
    UpdateCR1(env);
}

// From here on, the real deal.
// Interpreter_Paired.cpp, ps_div.
inline void ps_div(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const float ps0 =
      ForceSingle(env, NI_div(env, Bits<double>(a.ps0), Bits<double>(b.ps0)).value);
  const float ps1 =
      ForceSingle(env, NI_div(env, Bits<double>(a.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_res.
inline void ps_res(FPEnv& env, FPInst inst)
{
  // this code is based on the real hardware tests
  const double a = Bits<double>(env.ps[inst.FB].ps0);
  const double b = Bits<double>(env.ps[inst.FB].ps1);

  if (a == 0.0 || b == 0.0)
  {
    SetFPException(env, FPSCR_ZX);
    env.fpscr &= ~(3u << 17);
  }

  if (__builtin_isnan(a) || __builtin_isinf(a) || __builtin_isnan(b) || __builtin_isinf(b))
    env.fpscr &= ~(3u << 17);

  if (IsSNAN(a) || IsSNAN(b))
    SetFPException(env, FPSCR_VXSNAN);

  const double ps0 = ApproximateReciprocal(a);
  const double ps1 = ApproximateReciprocal(b);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, float(ps0));

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_rsqrte.
inline void ps_rsqrte(FPEnv& env, FPInst inst)
{
  const double ps0 = Bits<double>(env.ps[inst.FB].ps0);
  const double ps1 = Bits<double>(env.ps[inst.FB].ps1);

  if (ps0 == 0.0 || ps1 == 0.0)
  {
    SetFPException(env, FPSCR_ZX);
    env.fpscr &= ~(3u << 17);
  }

  if (ps0 < 0.0 || ps1 < 0.0)
  {
    SetFPException(env, FPSCR_VXSQRT);
    env.fpscr &= ~(3u << 17);
  }

  if (__builtin_isnan(ps0) || __builtin_isinf(ps0) || __builtin_isnan(ps1) || __builtin_isinf(ps1))
    env.fpscr &= ~(3u << 17);

  if (IsSNAN(ps0) || IsSNAN(ps1))
    SetFPException(env, FPSCR_VXSNAN);

  const float dst_ps0 = ForceSingle(env, ApproximateReciprocalSquareRoot(ps0));
  const float dst_ps1 = ForceSingle(env, ApproximateReciprocalSquareRoot(ps1));

  SetBoth(env.ps[inst.FD], dst_ps0, dst_ps1);
  UpdateFPRF(env, dst_ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

// PrimeHack for iPad: same input, result, rounding-mode and fused-tie guards as Interpreter_Paired.
#ifdef __aarch64__
template <u64 lo>
inline bool BothZeroOrFiniteAtLeast(float64x2_t v)
{
  const uint64x2_t magnitude =
      vandq_u64(vreinterpretq_u64_f64(v), vdupq_n_u64(DOUBLE_EXP | DOUBLE_FRAC));
  const uint64x2_t in_range =
      vcltq_u64(vsubq_u64(magnitude, vdupq_n_u64(lo)), vdupq_n_u64(DOUBLE_EXP - lo));
  const uint64x2_t ok = vorrq_u64(in_range, vceqzq_u64(magnitude));
  return (vgetq_lane_u64(ok, 0) & vgetq_lane_u64(ok, 1)) != 0;
}

// Input guard: every lane is +/-0 or a finite NORMAL double (>= the smallest normal double), so no
// subnormal, inf or NaN reaches the fast path. See the correctness contract above.
inline bool BothInputsOk(float64x2_t v)
{
  return BothZeroOrFiniteAtLeast<0x0010000000000000ULL>(v);
}

// Result guard: every lane is +/-0 or finite and >= the smallest normal single represented as a
// double (0x3810000000000000). That threshold is ForceSingle's OWN NI==1 flush comparison
// (Interpreter_FPUtils.h): at or above it the flush never fires and the converted single is normal,
// so the secondary FlushToZero(x) is a no-op too; below it (nonzero) NI=1 flushes while NI=0 does
// not, so we bail.
inline bool BothResultsOk(float64x2_t v)
{
  return BothZeroOrFiniteAtLeast<0x3810000000000000ULL>(v);
}

// SIMD Force25Bit for finite normal or +/-0 lanes (subnormal lanes are excluded by the predicate,
// so we only need the `else` branch of the scalar Force25Bit: integral = (integral & 0x...F8000000)
// + (integral & 0x8000000), which maps +/-0 to itself). Caller guarantees BothInputsOk.
inline float64x2_t Force25BitNormal(float64x2_t v)
{
  const uint64x2_t bits = vreinterpretq_u64_f64(v);
  const uint64x2_t kept = vandq_u64(bits, vdupq_n_u64(0xFFFFFFFFF8000000ULL));
  const uint64x2_t round = vandq_u64(bits, vdupq_n_u64(0x0000000008000000ULL));
  return vreinterpretq_f64_u64(vaddq_u64(kept, round));
}

// True if EITHER result lane lands exactly on the FMA even-tie (the only case where NI_madd_msub's
// single-precision once-rounding correction could differ by +/-1 ULP from a plain fma). Bail to scalar.
inline bool EitherEvenTie(float64x2_t v)
{
  const uint64x2_t bits = vreinterpretq_u64_f64(v);
  const uint64x2_t masked = vandq_u64(bits, vdupq_n_u64(0x000000001FFFFFFFULL));
  const uint64x2_t is_tie = vceqq_u64(masked, vdupq_n_u64(0x0000000010000000ULL));
  return (vgetq_lane_u64(is_tie, 0) | vgetq_lane_u64(is_tie, 1)) != 0;
}


enum class NeonOp { Add, Sub, Mul, Muls0, Muls1, Madd, Msub, Nmadd, Nmsub, Madds0, Madds1 };
template <NeonOp op>
inline bool PairedFast(FPEnv& env, FPInst inst)
{
  if (!env.host.neon || (env.fpscr & 3)) return false;
  constexpr bool add_sub = op == NeonOp::Add || op == NeonOp::Sub;
  constexpr bool multiply = op == NeonOp::Mul || op == NeonOp::Muls0 || op == NeonOp::Muls1;
  const auto load = [](PHPair p) { return Bits<float64x2_t>(p); };
  const auto a = load(env.ps[inst.FA]);
  if (!BothInputsOk(a)) return false;
  float64x2_t result;
  if constexpr (add_sub)
  {
    const auto b = load(env.ps[inst.FB]);
    if (!BothInputsOk(b)) return false;
    result = op == NeonOp::Add ? vaddq_f64(a, b) : vsubq_f64(a, b);
  }
  else
  {
    auto c = load(env.ps[inst.FC]);
    if constexpr (op == NeonOp::Muls0 || op == NeonOp::Madds0) c = vdupq_laneq_f64(c, 0);
    if constexpr (op == NeonOp::Muls1 || op == NeonOp::Madds1) c = vdupq_laneq_f64(c, 1);
    if (!BothInputsOk(c)) return false;
    c = Force25BitNormal(c);
    if constexpr (multiply)
      result = vmulq_f64(a, c);
    else
    {
      auto b = load(env.ps[inst.FB]);
      if (!BothInputsOk(b)) return false;
      if constexpr (op == NeonOp::Msub || op == NeonOp::Nmsub) b = vnegq_f64(b);
      result = vfmaq_f64(b, a, c);
      if (EitherEvenTie(result)) return false;
    }
  }
  if (!BothResultsOk(result)) return false;
  auto singles = vcvt_f32_f64(result);
  if constexpr (op == NeonOp::Nmadd || op == NeonOp::Nmsub) singles = vneg_f32(singles);
  const float first = vget_lane_f32(singles, 0);
  SetBoth(env.ps[inst.FD], first, vget_lane_f32(singles, 1));
  UpdateFPRF(env, first);
  if (inst.Rc) UpdateCR1(env);
  return true;
}
#endif

// Interpreter_Paired.cpp, ps_sub.
PH_COLD inline void ps_sub_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const float ps0 =
      ForceSingle(env, NI_sub(env, Bits<double>(a.ps0), Bits<double>(b.ps0)).value);
  const float ps1 =
      ForceSingle(env, NI_sub(env, Bits<double>(a.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_sub(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Sub>(env, inst)) return;
#endif
  ps_sub_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_add.
PH_COLD inline void ps_add_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  const float ps0 =
      ForceSingle(env, NI_add(env, Bits<double>(a.ps0), Bits<double>(b.ps0)).value);
  const float ps1 =
      ForceSingle(env, NI_add(env, Bits<double>(a.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_add(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Add>(env, inst)) return;
#endif
  ps_add_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_mul.
PH_COLD inline void ps_mul_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& c = env.ps[inst.FC];

  const double c0 = Force25Bit(Bits<double>(c.ps0));
  const double c1 = Force25Bit(Bits<double>(c.ps1));

  const float ps0 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps0), c0).value);
  const float ps1 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps1), c1).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_mul(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Mul>(env, inst)) return;
#endif
  ps_mul_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_msub.
PH_COLD inline void ps_msub_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 = ForceSingle(
      env,
      NI_msub<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0)).value);
  const float ps1 = ForceSingle(
      env,
      NI_msub<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_msub(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Msub>(env, inst)) return;
#endif
  ps_msub_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_madd.
PH_COLD inline void ps_madd_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0)).value);
  const float ps1 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_madd(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Madd>(env, inst)) return;
#endif
  ps_madd_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_nmsub.
PH_COLD inline void ps_nmsub_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float tmp0 = ForceSingle(
      env,
      NI_msub<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0)).value);
  const float tmp1 = ForceSingle(
      env,
      NI_msub<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps1), Bits<double>(b.ps1)).value);

  const float ps0 = __builtin_isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = __builtin_isnan(tmp1) ? tmp1 : -tmp1;

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_nmsub(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Nmsub>(env, inst)) return;
#endif
  ps_nmsub_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_nmadd.
PH_COLD inline void ps_nmadd_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float tmp0 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0)).value);
  const float tmp1 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps1), Bits<double>(b.ps1)).value);

  const float ps0 = __builtin_isnan(tmp0) ? tmp0 : -tmp0;
  const float ps1 = __builtin_isnan(tmp1) ? tmp1 : -tmp1;

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_nmadd(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Nmadd>(env, inst)) return;
#endif
  ps_nmadd_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_sum0.
inline void ps_sum0(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 =
      ForceSingle(env, NI_add(env, Bits<double>(a.ps0), Bits<double>(b.ps1)).value);
  const float ps1 = ForceSingle(env, Bits<double>(c.ps1));

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_sum1.
inline void ps_sum1(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 = ForceSingle(env, Bits<double>(c.ps0));
  const float ps1 =
      ForceSingle(env, NI_add(env, Bits<double>(a.ps0), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps1);

  if (inst.Rc)
    UpdateCR1(env);
}

// Interpreter_Paired.cpp, ps_muls0.
PH_COLD inline void ps_muls0_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& c = env.ps[inst.FC];

  const double c0 = Force25Bit(Bits<double>(c.ps0));
  const float ps0 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps0), c0).value);
  const float ps1 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps1), c0).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_muls0(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Muls0>(env, inst)) return;
#endif
  ps_muls0_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_muls1.
PH_COLD inline void ps_muls1_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& c = env.ps[inst.FC];

  const double c1 = Force25Bit(Bits<double>(c.ps1));
  const float ps0 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps0), c1).value);
  const float ps1 = ForceSingle(env, NI_mul(env, Bits<double>(a.ps1), c1).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_muls1(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Muls1>(env, inst)) return;
#endif
  ps_muls1_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_madds0.
PH_COLD inline void ps_madds0_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps0), Bits<double>(b.ps0)).value);
  const float ps1 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps0), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_madds0(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Madds0>(env, inst)) return;
#endif
  ps_madds0_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_madds1.
PH_COLD inline void ps_madds1_scalar(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];
  const auto& c = env.ps[inst.FC];

  const float ps0 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps0), Bits<double>(c.ps1), Bits<double>(b.ps0)).value);
  const float ps1 = ForceSingle(
      env,
      NI_madd<true>(env, Bits<double>(a.ps1), Bits<double>(c.ps1), Bits<double>(b.ps1)).value);

  SetBoth(env.ps[inst.FD], ps0, ps1);
  UpdateFPRF(env, ps0);

  if (inst.Rc)
    UpdateCR1(env);
}

inline void ps_madds1(FPEnv& env, FPInst inst)
{
#ifdef __aarch64__
  if (PairedFast<NeonOp::Madds1>(env, inst)) return;
#endif
  ps_madds1_scalar(env, inst);
}

// Interpreter_Paired.cpp, ps_cmpu0.
inline void ps_cmpu0(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareUnordered(env, inst, Bits<double>(a.ps0), Bits<double>(b.ps0));
}

// Interpreter_Paired.cpp, ps_cmpo0.
inline void ps_cmpo0(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareOrdered(env, inst, Bits<double>(a.ps0), Bits<double>(b.ps0));
}

// Interpreter_Paired.cpp, ps_cmpu1.
inline void ps_cmpu1(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareUnordered(env, inst, Bits<double>(a.ps1), Bits<double>(b.ps1));
}

// Interpreter_Paired.cpp, ps_cmpo1.
inline void ps_cmpo1(FPEnv& env, FPInst inst)
{
  const auto& a = env.ps[inst.FA];
  const auto& b = env.ps[inst.FB];

  Helper_FloatCompareOrdered(env, inst, Bits<double>(a.ps1), Bits<double>(b.ps1));
}


// Interpreter_LoadStorePaired.cpp, ReadPair/WritePair and LoadAndDequantize.
// A pair is one MMU transaction. Splitting it changes fault and MMIO behavior.
template <class T> struct Twice;
template <> struct Twice<u8> { using type = u16; };
template <> struct Twice<u16> { using type = u32; };
template <> struct Twice<u32> { using type = u64; };

template <class T, class Convert>
inline void LoadPair(PHPair& dst, u8* addr, bool single, Convert convert)
{
  u64 first, second;
  if (single)
  {
    first = convert(Read<T>(addr));
    second = Bits<u64>(1.0);
  }
  else
  {
    auto pair = Read<typename Twice<T>::type>(addr);
    first = convert(T(pair >> (8 * sizeof(T))));
    second = convert(T(pair));
  }
  dst = {first, second};
}

template <class T, class Convert>
inline void StorePair(PHPair src, u8* addr, bool single, Convert convert)
{
  using Wide = typename Twice<T>::type;
  T first = convert(src.ps0);
  if (single)
    Write<T>(addr, first);
  else
    Write<Wide>(addr, (Wide(first) << (8 * sizeof(T))) | T(convert(src.ps1)));
}

// Interpreter_LoadStorePaired.cpp, m_quantizeTable/m_dequantizeTable, ScaleAndClamp.
template <class Storage, class Number, bool store>
inline void ScaledPair(PHPair& pair, u8* addr, bool single, int scale)
{
  const float factor = Bits<float>(u32(127 + (store ? scale : -scale)) << 23);
  if constexpr (store)
  {
    StorePair<Storage>(pair, addr, single, [factor](u64 bits) {
      const float f = float(Bits<double>(bits)) * factor;
      constexpr int width = 8 * sizeof(Number);
      constexpr int low = Number(-1) < Number(0) ? -(1 << (width - 1)) : 0;
      constexpr int high = (1 << width) - 1 + low;
      return Storage(Number(f < low ? float(low) : f > high ? float(high) : f));
    });
  }
  else
  {
    LoadPair<Storage>(pair, addr, single, [factor](Storage v) {
      return Bits<u64>(double(float(Number(v)) * factor));
    });
  }
}

// Interpreter::psq_l/lu/st/stu and indexed variants. False delegates invalid GQRs and
// NaN-to-integer conversions to the original handler before issuing any access.
template <bool store>
inline bool Quantized(PHPair& PH_RESTRICT pair, u8* PH_RESTRICT addr, u32 gqr, bool single)
{
  const u32 spec = store ? gqr : gqr >> 16;
  const u32 type = spec & 7;
  const int scale = int(((spec >> 8) & 63) ^ 32) - 32;
  if (type == 0)
  {
    if constexpr (store)
      StorePair<u32>(pair, addr, single, ConvertToSingleFTZ);
    else
      LoadPair<u32>(pair, addr, single, ConvertToDouble);
    return true;
  }
  if (store && (__builtin_isnan(Bits<double>(pair.ps0)) ||
                (!single && __builtin_isnan(Bits<double>(pair.ps1)))))
    return false;
  switch (type)
  {
  case 4: ScaledPair<u8, u8, store>(pair, addr, single, scale); break;
  case 5: ScaledPair<u16, u16, store>(pair, addr, single, scale); break;
  case 6: ScaledPair<u8, signed char, store>(pair, addr, single, scale); break;
  case 7: ScaledPair<u16, short, store>(pair, addr, single, scale); break;
  default: return false;
  }
  return true;
}

PH_COLD inline u32 CheckedFault(PHState& s, const PHAotHost& h, u32 start, u32 pc, u32 cycles)
{
  s.pc = pc;
  s.npc = pc + 4;
  s.downcount = s32(u32(s.downcount) - cycles);
  if (h.profile_block)
    h.profile_block(h.context, start, cycles);
  h.check_exceptions(h.context);
  return PH_DISPATCH;
}

template <class Spill, class Reload>
PH_COLD inline void HostOp(PHState& s, void* context, u32 pc, u32 word,
                           void (*fn)(void*, u32), Spill& spill, Reload& reload)
{
  spill();
  s.pc = pc;
  s.npc = pc + 4;
  fn(context, word);
  reload();
}

// Accounting callbacks only observe the supplied literals and update counters.
PH_COLD inline void ChargeCounters(u32 features, const PHAotHost& h, u32 start, u32 cycles,
                                   u32 ls, u32 fp)
{
  if (features & PH_PERFMON)
    h.performance_monitor(h.context, cycles, ls, fp);
  if (h.profile_block)
    h.profile_block(h.context, start, cycles);
}

inline void Charge(s32& downcount, u32 features, const PHAotHost& h, u32 start, u32 cycles,
                   u32 ls, u32 fp)
{
  downcount = s32(u32(downcount) - cycles);
  if ((features & PH_PERFMON) || h.profile_block) [[unlikely]]
    ChargeCounters(features, h, start, cycles, ls, fp);
}
}  // namespace ph
