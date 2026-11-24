// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/Interpreter/Interpreter.h"

#include <cmath>
#include <utility>

#include "Common/CommonTypes.h"
#include "Common/FloatUtils.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPInstructions.h"
#include "Core/PowerPC/PowerPC.h"

// PrimeHack for iPad: paired compares and scalar compares use the same FPSCR/CR update.
void Interpreter::Helper_FloatCompareOrdered(PowerPC::PowerPCState& ppc_state,
                                             UGeckoInstruction inst, double fa, double fb)
{
  InterpreterFP::Compare<true>(ppc_state, inst.CRFD, fa, fb);
}

void Interpreter::Helper_FloatCompareUnordered(PowerPC::PowerPCState& ppc_state,
                                               UGeckoInstruction inst, double fa, double fb)
{
  InterpreterFP::Compare<false>(ppc_state, inst.CRFD, fa, fb);
}

void Interpreter::fcmpo(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fcmpo<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fcmpu(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fcmpu<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fctiwx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  InterpreterFP::ConvertToInteger(ppc_state, inst,
                                  static_cast<InterpreterFP::RoundingMode>(ppc_state.fpscr.RN.Value()));
}

void Interpreter::fctiwzx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fctiwzx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fmrx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmrx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fabsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fabsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnabsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnabsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnegx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnegx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fselx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fselx<true>(interpreter.m_ppc_state, inst);
}

// !!! warning !!!
// PS1 must be set to the value of PS0 or DragonballZ will be f**ked up
// PS1 is said to be undefined
void Interpreter::frspx(Interpreter& interpreter, UGeckoInstruction inst)  // round to single
{
  InterpreterFP::frspx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fmulx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmulx<true>(interpreter.m_ppc_state, inst);
}
void Interpreter::fmulsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmulsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fmaddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmaddx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fmaddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmaddsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::faddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::faddx<true>(interpreter.m_ppc_state, inst);
}
void Interpreter::faddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::faddsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fdivx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fdivx<true>(interpreter.m_ppc_state, inst);
}
void Interpreter::fdivsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fdivsx<true>(interpreter.m_ppc_state, inst);
}

// Single precision only.
void Interpreter::fresx(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();

  const auto compute_result = [&ppc_state, inst](double value) {
    const double result = Common::ApproximateReciprocal(value);
    ppc_state.ps[inst.FD].Fill(result);
    ppc_state.UpdateFPRFSingle(float(result));
  };

  if (b == 0.0)
  {
    SetFPException(ppc_state, FPSCR_ZX);
    ppc_state.fpscr.ClearFIFR();

    if (ppc_state.fpscr.ZE == 0)
      compute_result(b);
  }
  else if (Common::IsSNAN(b))
  {
    SetFPException(ppc_state, FPSCR_VXSNAN);
    ppc_state.fpscr.ClearFIFR();

    if (ppc_state.fpscr.VE == 0)
      compute_result(b);
  }
  else
  {
    if (std::isnan(b) || std::isinf(b))
      ppc_state.fpscr.ClearFIFR();

    compute_result(b);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

void Interpreter::frsqrtex(Interpreter& interpreter, UGeckoInstruction inst)
{
  auto& ppc_state = interpreter.m_ppc_state;
  const double b = ppc_state.ps[inst.FB].PS0AsDouble();

  const auto compute_result = [&ppc_state, inst](double value) {
    const double result = Common::ApproximateReciprocalSquareRoot(value);
    ppc_state.ps[inst.FD].SetPS0(result);
    ppc_state.UpdateFPRFDouble(result);
  };

  if (b < 0.0)
  {
    SetFPException(ppc_state, FPSCR_VXSQRT);
    ppc_state.fpscr.ClearFIFR();

    if (ppc_state.fpscr.VE == 0)
      compute_result(b);
  }
  else if (b == 0.0)
  {
    SetFPException(ppc_state, FPSCR_ZX);
    ppc_state.fpscr.ClearFIFR();

    if (ppc_state.fpscr.ZE == 0)
      compute_result(b);
  }
  else if (Common::IsSNAN(b))
  {
    SetFPException(ppc_state, FPSCR_VXSNAN);
    ppc_state.fpscr.ClearFIFR();

    if (ppc_state.fpscr.VE == 0)
      compute_result(b);
  }
  else
  {
    if (std::isnan(b) || std::isinf(b))
      ppc_state.fpscr.ClearFIFR();

    compute_result(b);
  }

  if (inst.Rc)
    ppc_state.UpdateCR1();
}

void Interpreter::fmsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmsubx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fmsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fmsubsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnmaddx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnmaddx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnmaddsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnmaddsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnmsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnmsubx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fnmsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fnmsubsx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fsubx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fsubx<true>(interpreter.m_ppc_state, inst);
}

void Interpreter::fsubsx(Interpreter& interpreter, UGeckoInstruction inst)
{
  InterpreterFP::fsubsx<true>(interpreter.m_ppc_state, inst);
}
