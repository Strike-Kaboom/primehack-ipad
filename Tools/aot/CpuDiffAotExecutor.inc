// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// PrimeHack for iPad: include after DolphinExecutor in CpuDiffTest.cpp.
// Include Core/PowerPC/CachedInterpreter/CachedInterpreter.h at file scope.
class AotExecutor final : public Executor
{
public:
  AotExecutor(Core::System& system, bool fprf, bool enables)
      : m_system(system), m_cached(system, PowerPC::CPUCore::CachedInterpreter, fprf, enables)
  {
  }

  std::string_view Name() const override { return "AOT"; }

  State Run(const Program& program) override
  {
    Install(m_system, program);
    m_native_records = 0;
    const u32 end = CODE + static_cast<u32>(4 * program.words.size());
    auto& cpu = m_system.GetPPCState();
    auto& core = static_cast<CachedInterpreter&>(*m_system.GetJitInterface().GetCore());
    for (u32 step = 0; step < STEP_LIMIT && cpu.pc >= CODE && cpu.pc < end && !(cpu.pc & 3);
         ++step)
      m_native_records += core.SingleStepAotForTest();
    return Capture(m_system, program);
  }

  u32 NativeRecords() const { return m_native_records; }

private:
  Core::System& m_system;
  DolphinExecutor m_cached;
  u32 m_native_records = 0;
};
