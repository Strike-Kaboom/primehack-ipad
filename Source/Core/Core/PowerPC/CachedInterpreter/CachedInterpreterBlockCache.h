// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

class CachedInterpreterBlockCache final : public JitBaseBlockCache
{
public:
  explicit CachedInterpreterBlockCache(JitBase& jit);

  void Init() override;

  void DestroyBlock(JitBlock& block) override;

  void RecordTargetTranslations(const JitBlock& block, std::span<const PPCAnalyst::CodeOp> ops);
  bool CanCacheTarget(const u8* entry) const
  {
    return m_page_table_targets.empty() || !m_page_table_targets.contains(entry);
  }

  void ClearRangesToFree();

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFree() const
  {
    return m_ranges_to_free_on_next_codegen;
  }

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;

  std::unordered_set<const u8*> m_page_table_targets;
  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen;
};
