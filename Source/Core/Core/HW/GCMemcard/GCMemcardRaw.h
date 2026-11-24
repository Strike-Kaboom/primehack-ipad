// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/HookableEvent.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardBase.h"

class PointerWrap;

class MemoryCard : public MemoryCardBase
{
public:
  MemoryCard(std::string filename, ExpansionInterface::Slot card_slot,
             u16 size_mbits = Memcard::MBIT_SIZE_MEMORY_CARD_2043);
  ~MemoryCard() override;
  void FlushThread();
  void FlushNow();
  void MakeDirty();

  s32 Read(u32 src_address, s32 length, u8* dest_address) override;
  s32 Write(u32 dest_address, s32 length, const u8* src_address) override;
  void ClearBlock(u32 address) override;
  void ClearAll() override;
  void DoState(PointerWrap& p) override;

private:
  bool IsAddressInBounds(u32 address, u32 length) const
  {
    const u64 end_address = static_cast<u64>(address) + static_cast<u64>(length);
    return end_address <= static_cast<u64>(m_memory_card_size);
  }

  bool WriteToDisk();

  std::string m_filename;
  std::unique_ptr<u8[]> m_memcard_data;
  std::unique_ptr<u8[]> m_flush_buffer;
  std::thread m_flush_thread;
  std::mutex m_flush_mutex;
  std::mutex m_write_mutex;
  Common::Event m_flush_trigger;
  Common::Flag m_dirty;
  Common::EventHook m_flush_unsaved_hook;
  u32 m_memory_card_size;
};
