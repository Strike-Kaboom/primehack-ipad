// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <utility>

#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"

// PrimeHack for iPad: share the architectural operation with the interpreter. Decoded registers
// are read from a compact tape record. The generic instruction still supplies its live Rc bit.
namespace InterpreterFP
{
template <bool record>
struct Registers
{
  u8 FD, FA, FB, FC;
  static constexpr bool Rc = record;
};
using Decoded = Registers<false>;

inline u32 CompareField(UGeckoInstruction inst)
{
  return inst.CRFD;
}
inline u32 CompareField(Decoded inst)
{
  return inst.FD;
}

// Apply current rounding mode
enum class RoundingMode
{
  Nearest = 0b00,
  TowardsZero = 0b01,
  TowardsPositiveInfinity = 0b10,
  TowardsNegativeInfinity = 0b11
};

inline void SetFI(PowerPC::PowerPCState& ppc_state, u32 FI)
{
  if (FI != 0)
  {
    SetFPException(ppc_state, FPSCR_XX);
  }
  ppc_state.fpscr.FI = FI;
}

// Round a number to an integer in the same direction as the CPU rounding mode,
// without setting any CPU flags or being careful about NaNs
inline double RoundToIntegerMode(double number)
{
  // This value is 2^52 -- The first number in which double precision floating point
  // numbers can only store subsequent integers, and no longer any decimals
  // This keeps the sign of the unrounded value because it needs to scale it
  // upwards when added
  const double int_precision = std::copysign(4503599627370496.0, number);

  // By adding this value to the original number,
  // it will be forced to decide a integer to round to
  // This rounding will be the same as the CPU rounding mode
  return (number + int_precision) - int_precision;
}

// Note that the convert to integer operation is defined
// in Appendix C.4.2 in PowerPC Microprocessor Family:
// The Programming Environments Manual for 32 and 64-bit Microprocessors
template <typename Inst>
inline void ConvertToInteger(PowerPC::PowerPCState& ppc_state, Inst inst, RoundingMode rounding_mode)
{
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
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
    rounded = std::trunc(b);
    break;
  case RoundingMode::TowardsPositiveInfinity:
    rounded = std::ceil(b);
    break;
  case RoundingMode::TowardsNegativeInfinity:
    rounded = std::floor(b);
    break;
  default:
    std::unreachable();
  }

  if (std::isnan(b))
  {
    if (Common::IsSNAN(b))
      SetFPException(ppc_state, FPSCR_VXSNAN);

    value = 0x80000000;
    SetFPException(ppc_state, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else if (rounded >= static_cast<double>(0x80000000))
  {
    // Positive large operand or +inf
    value = 0x7fffffff;
    SetFPException(ppc_state, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else if (rounded < -static_cast<double>(0x80000000))
  {
    // Negative large operand or -inf
    value = 0x80000000;
    SetFPException(ppc_state, FPSCR_VXCVI);
    exception_occurred = true;
  }
  else
  {
    s32 signed_value = static_cast<s32>(rounded);
    value = static_cast<u32>(signed_value);
    const double di = static_cast<double>(signed_value);
    if (di == b)
    {
      ppc_state.fpscr.ClearFIFR();
    }
    else
    {
      // Also sets FPSCR[XX]
      SetFI(ppc_state, 1);
      ppc_state.fpscr.FR = fabs(di) > fabs(b);
    }
  }

  if (exception_occurred)
  {
    ppc_state.fpscr.ClearFIFR();
  }

  if (!exception_occurred || ppc_state.fpscr.VE == 0)
  {
    // Based on HW tests
    // FPRF is not affected
    u64 result = 0xfff8000000000000ull | value;
    if (value == 0 && std::signbit(b))
      result |= 0x100000000ull;

    ppc_state.ps[inst.FD].SetPS0(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool ordered>
inline u32 Compare(PowerPC::PowerPCState& ppc_state, u32 crfd, double fa, double fb)
{
  FPCC compare_result;

  if (std::isnan(fa) || std::isnan(fb))
  {
    compare_result = FPCC::FU;
    if (Common::IsSNAN(fa) || Common::IsSNAN(fb))
    {
      SetFPException(ppc_state, FPSCR_VXSNAN);
      if constexpr (ordered)
      {
        if (ppc_state.fpscr.VE == 0)
          SetFPException(ppc_state, FPSCR_VXVC);
      }
    }
    else if constexpr (ordered)  // QNaN
    {
      SetFPException(ppc_state, FPSCR_VXVC);
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
  ppc_state.fpscr.FPRF = (ppc_state.fpscr.FPRF & ~FPCC_MASK) | compare_value;

  ppc_state.cr.SetField(crfd, compare_value);
  return compare_value;
}

template <bool fprf, typename Inst>
inline void fmulsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& c = ppc_state.ps[inst.FC];

  const double c_value = Force25Bit(c.PS0AsDouble());
  const FPResult product = NI_mul(ppc_state, a.PS0AsDouble(), c_value);

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(ppc_state.fpscr, product.value);

    ppc_state.ps[inst.FD].Fill(result);
    ppc_state.fpscr.FI = 0;
    ppc_state.fpscr.FR = 0;
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void faddsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  const FPResult sum = NI_add(ppc_state, a.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || sum.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(ppc_state.fpscr, sum.value);
    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fsubsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  const FPResult difference = NI_sub(ppc_state, a.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || difference.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(ppc_state.fpscr, difference.value);
    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmulx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product = NI_mul(ppc_state, a.PS0AsDouble(), c.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(ppc_state.fpscr, product.value);

    ppc_state.ps[inst.FD].SetPS0(result);
    ppc_state.fpscr.FI = 0;  // are these flags important?
    ppc_state.fpscr.FR = 0;
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void faddx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  const FPResult sum = NI_add(ppc_state, a.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || sum.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(ppc_state.fpscr, sum.value);
    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fsubx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  const FPResult difference = NI_sub(ppc_state, a.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || difference.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(ppc_state.fpscr, difference.value);
    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmaddsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(ppc_state.fpscr, product.value);

    ppc_state.ps[inst.FD].Fill(result);
    ppc_state.fpscr.FI = product.value != result;
    ppc_state.fpscr.FR = 0;
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmsubsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const float result = ForceSingle(ppc_state.fpscr, product.value);
    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnmaddsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_madd<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const float tmp = ForceSingle(ppc_state.fpscr, product.value);
    const float result = std::isnan(tmp) ? tmp : -tmp;

    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnmsubsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_msub<true>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const float tmp = ForceSingle(ppc_state.fpscr, product.value);
    const float result = std::isnan(tmp) ? tmp : -tmp;

    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmaddx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_madd<false>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(ppc_state.fpscr, product.value);
    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmsubx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_msub<false>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const double result = ForceDouble(ppc_state.fpscr, product.value);
    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnmaddx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_madd<false>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const double tmp = ForceDouble(ppc_state.fpscr, product.value);
    const double result = std::isnan(tmp) ? tmp : -tmp;

    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnmsubx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const FPResult product =
      NI_msub<false>(ppc_state, a.PS0AsDouble(), c.PS0AsDouble(), b.PS0AsDouble());

  if (ppc_state.fpscr.VE == 0 || product.HasNoInvalidExceptions())
  {
    const double tmp = ForceDouble(ppc_state.fpscr, product.value);
    const double result = std::isnan(tmp) ? tmp : -tmp;

    ppc_state.ps[inst.FD].SetPS0(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFDouble(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void frspx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();
  const float rounded = ForceSingle(ppc_state.fpscr, b);

  if (std::isnan(b))
  {
    const bool is_snan = Common::IsSNAN(b);

    if (is_snan)
      SetFPException(ppc_state, FPSCR_VXSNAN);

    if (!is_snan || ppc_state.fpscr.VE == 0)
    {
      ppc_state.ps[inst.FD].Fill(rounded);
      if constexpr (fprf)
        ppc_state.UpdateFPRFSingle(rounded);
    }

    ppc_state.fpscr.ClearFIFR();
  }
  else
  {
    SetFI(ppc_state, b != rounded);
    ppc_state.fpscr.FR = fabs(rounded) > fabs(b);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(rounded);
    ppc_state.ps[inst.FD].Fill(rounded);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fctiwzx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  ConvertToInteger(ppc_state, inst, RoundingMode::TowardsZero);
}

template <bool fprf, typename Inst>
inline void fdivsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  const FPResult quotient = NI_div(ppc_state, a.PS0AsDouble(), b.PS0AsDouble());
  const bool not_divide_by_zero = ppc_state.fpscr.ZE == 0 || quotient.exception != FPSCR_ZX;
  const bool not_invalid = ppc_state.fpscr.VE == 0 || quotient.HasNoInvalidExceptions();

  if (not_divide_by_zero && not_invalid)
  {
    const float result = ForceSingle(ppc_state.fpscr, quotient.value);
    ppc_state.ps[inst.FD].Fill(result);
    if constexpr (fprf)
      ppc_state.UpdateFPRFSingle(result);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fmrx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64());

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fabsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  ppc_state.ps[inst.FD].SetPS0(fabs(ppc_state.ps[inst.FB].PS0AsDouble()));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnegx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64() ^ (UINT64_C(1) << 63));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fnabsx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  ppc_state.ps[inst.FD].SetPS0(ppc_state.ps[inst.FB].PS0AsU64() | (UINT64_C(1) << 63));

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fselx(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  ppc_state.ps[inst.FD].SetPS0((a.PS0AsDouble() >= -0.0) ? c.PS0AsDouble() : b.PS0AsDouble());

  // This is a binary instruction. Does not alter FPSCR
  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void fcmpo(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  Compare<true>(ppc_state, CompareField(inst), a.PS0AsDouble(), b.PS0AsDouble());
}

template <bool fprf, typename Inst>
inline void fcmpu(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  Compare<false>(ppc_state, CompareField(inst), a.PS0AsDouble(), b.PS0AsDouble());
}

template <bool fprf, typename Inst>
void ps_madd(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_madd<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_madd<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_mul(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_mul<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_mul<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_madds0(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_madds0<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_madds0<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_madds1(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_madds1<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_madds1<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_muls0(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_muls0<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_muls0<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_muls1(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_muls1<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_muls1<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_add(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_add<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_add<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_sub(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_sub<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_sub<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
void ps_msub(PowerPC::PowerPCState& ppc_state, Inst inst);
extern template void ps_msub<false>(PowerPC::PowerPCState&, Decoded);
extern template void ps_msub<true>(PowerPC::PowerPCState&, Decoded);

template <bool fprf, typename Inst>
inline void ps_merge00(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  ppc_state.ps[inst.FD].SetBoth(a.PS0AsDouble(), b.PS0AsDouble());

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void ps_merge01(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  ppc_state.ps[inst.FD].SetBoth(a.PS0AsDouble(), b.PS1AsDouble());

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void ps_merge10(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  ppc_state.ps[inst.FD].SetBoth(a.PS1AsDouble(), b.PS0AsDouble());

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void ps_merge11(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];

  ppc_state.ps[inst.FD].SetBoth(a.PS1AsDouble(), b.PS1AsDouble());

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

template <bool fprf, typename Inst>
inline void ps_sum0(PowerPC::PowerPCState& ppc_state, Inst inst)
{
  const auto& a = ppc_state.ps[inst.FA];
  const auto& b = ppc_state.ps[inst.FB];
  const auto& c = ppc_state.ps[inst.FC];

  const float ps0 =
      ForceSingle(ppc_state.fpscr, NI_add(ppc_state, a.PS0AsDouble(), b.PS1AsDouble()).value);
  const float ps1 = ForceSingle(ppc_state.fpscr, c.PS1AsDouble());

  ppc_state.ps[inst.FD].SetBoth(ps0, ps1);
  if constexpr (fprf)
    ppc_state.UpdateFPRFSingle(ps0);

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

}  // namespace InterpreterFP
