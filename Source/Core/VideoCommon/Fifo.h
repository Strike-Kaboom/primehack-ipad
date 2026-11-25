// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>

#include "Common/BlockingLoop.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Event.h"
#include "Common/Flag.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}

namespace Fifo
{
// Used for diagnostics.
enum class SyncGPUReason
{
  Other,
  Wraparound,
  EFBPoke,
  PerfQuery,
  BBox,
  Swap,
  AuxSpace,
};

class FifoManager final
{
public:
  explicit FifoManager(Core::System& system);
  FifoManager(const FifoManager& other) = delete;
  FifoManager(FifoManager&& other) = delete;
  FifoManager& operator=(const FifoManager& other) = delete;
  FifoManager& operator=(FifoManager&& other) = delete;
  ~FifoManager();

  void Init();
  void Shutdown();
  void Prepare();  // Must be called from the CPU thread.
  void DoState(PointerWrap& f);
  void PauseAndLock();
  void RestoreState(bool was_running);
  void UpdateWantDeterminism(bool want);
  bool UseDeterministicGPUThread() const { return m_use_deterministic_gpu_thread; }
  bool UseSyncGPU() const { return m_config_sync_gpu; }

  // In deterministic GPU thread mode this waits for the GPU to be done with pending work.
  void SyncGPU(SyncGPUReason reason, bool may_move_read_ptr = true);

  // In single core mode, this runs the GPU for a single slice.
  // In dual core mode, this synchronizes with the GPU thread.
  void SyncGPUForRegisterAccess();

  void PushFifoAuxBuffer(const void* ptr, size_t size);
  void* PopFifoAuxBuffer(size_t size);

  void FlushGpu();
  void RunGpu();
  void GpuMaySleep();
  void RunGpuLoop();
  void ExitGpuLoop();
  void EmulatorState(bool running);
  void ResetVideoBuffer();

private:
  void RefreshConfig();
  void ReadDataFromFifo(u32 read_ptr);
  void ReadDataFromFifoOnCPU(u32 read_ptr);
  int RunGpuOnCpu(int ticks);
  int WaitForGpuThread(int ticks);
  static void SyncGPUCallback(Core::System& system, u64 ticks, s64 cyclesLate);

  static constexpr u32 FIFO_SIZE = 2 * 1024 * 1024;

  Common::BlockingLoop m_gpu_mainloop;

  Common::Flag m_emu_running_state;

  // Most of this array is unlikely to be faulted in...
  u8 m_fifo_aux_data[FIFO_SIZE]{};
  u8* m_fifo_aux_write_ptr = nullptr;
  u8* m_fifo_aux_read_ptr = nullptr;

  // This could be in SConfig, but it depends on multiple settings
  // and can change at runtime.
  bool m_use_deterministic_gpu_thread = false;

  CoreTiming::EventType* m_event_sync_gpu = nullptr;

  // STATE_TO_SAVE
  u8* m_video_buffer = nullptr;
  u8* m_video_buffer_read_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_write_ptr = nullptr;
  std::atomic<u8*> m_video_buffer_seen_ptr = nullptr;
  u8* m_video_buffer_pp_read_ptr = nullptr;
  // The read_ptr is always owned by the GPU thread.  In normal mode, so is the
  // write_ptr, despite it being atomic.  In deterministic GPU thread mode,
  // things get a bit more complicated:
  // - The seen_ptr is written by the GPU thread, and points to what it's already
  // processed as much of as possible - in the case of a partial command which
  // caused it to stop, not the same as the read ptr.  It's written by the GPU,
  // under the lock, and updating the cond.
  // - The write_ptr is written by the CPU thread after it copies data from the
  // FIFO.  Maybe someday it will be under the lock.  For now, because RunGpuLoop
  // polls, it's just atomic.
  // - The pp_read_ptr is the CPU preprocessing version of the read_ptr.

  std::atomic<int> m_sync_ticks = 0;
  bool m_syncing_suspended = false;

  // iCube: idle-poll backoff for the dual-core video thread (see RunGpuLoop). After
  // GPU_IDLE_SPIN_POLLS consecutive payload iterations that drained nothing, allow
  // BlockingLoop to park on m_new_work_event. Only touched on the video thread.
  static constexpr u32 GPU_IDLE_SPIN_POLLS = 16;
  // PrimeHack for iPad: once idle, keep polling this long before letting BlockingLoop park.
  // Parking costs the video thread nothing, but the CPU thread's next gather-pipe burst
  // (RunGpu -> Wakeup) then has to signal a condition variable that has a blocked waiter, a
  // system call on the emulation thread, and the video thread only resumes after a scheduler
  // wake. Bursts arrive microseconds apart while a frame is being submitted, so parking after a
  // few idle polls put that cost on almost every gap between draws. Long idle stretches (the CPU
  // thread in game logic or asleep for frame pacing, a paused game) still park. 50 us is about ten
  // times the usual gap between draws; raising it trades video-thread heat for fewer wakes.
  static constexpr std::chrono::microseconds GPU_IDLE_PARK_DELAY{50};
  u32 m_gpu_idle_polls = 0;
  std::chrono::steady_clock::time_point m_gpu_idle_since{};
  Common::Event m_sync_wakeup_event;

  std::optional<Config::ConfigChangedCallbackID> m_config_callback_id = std::nullopt;
  bool m_config_sync_gpu = false;
  int m_config_sync_gpu_max_distance = 0;
  int m_config_sync_gpu_min_distance = 0;
  float m_config_sync_gpu_overclock = 0.0f;

  Core::System& m_system;
};

bool AtBreakpoint(Core::System& system);
}  // namespace Fifo
