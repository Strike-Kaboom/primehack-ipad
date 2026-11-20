// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <memory>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"
#include "Core/PowerPC/Aot/AotAbi.h"

namespace PPCAnalyst
{
class PPCAnalyzer;
}
namespace Core
{
class System;
}
namespace PowerPC
{
struct PowerPCState;
namespace Aot
{
class Runtime
{
public:
  Runtime() = default;
  using ProfileBlock = void (*)(void*, PHU32, PHU32);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  void Init(Core::System& system, ProfileBlock profile = nullptr);
  void Shutdown();
  bool Loaded() const { return m_module != nullptr; }
  std::span<const PHAotEntry> Candidates(u32 pc) const;
  // CPU thread. Only a successful match publishes the complete set of physical ranges.
  const PHAotEntry* Bind(u32 pc, Common::RangeSet<u32>& ranges,
                         const PPCAnalyst::PPCAnalyzer& analyzer, size_t max_ops);
  // PrimeHack for iPad: bind verifies bytes, Publish follows successful block finalization.
  void Publish(const PHAotEntry& entry, PHU32 flags);
  void Invalidate(u32 pc, PHU32 flags);
  void NotifyTapeReuse()
  {
    if (Loaded())
      ++m_generation;
  }
  PHU64 Generation() const { return m_generation; }
  bool Eligible() const;
  u64 Executions() const { return m_stats ? m_stats->executions.load(std::memory_order_relaxed) : 0; }
  u32 Execute(PowerPCState& state, const PHAotEntry& entry);
  struct Counters
  {
    std::atomic<u64> entries{}, bound{}, mismatches{}, rejected{}, executions{}, fallbacks{};
  };

private:
  Core::System* m_system = nullptr;
  PHU64 m_generation = 1;
  std::unique_ptr<const PHAotEntry*[]> m_bound;
  std::shared_ptr<Counters> m_stats;
  void* m_library = nullptr;
  const PHAotModule* m_module = nullptr;
  PHAotHost m_host{};
};

std::string StatsReport();
}  // namespace Aot
}  // namespace PowerPC
