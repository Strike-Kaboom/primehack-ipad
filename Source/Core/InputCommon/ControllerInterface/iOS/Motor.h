// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <mutex>

#include "InputCommon/ControllerInterface/ControllerInterface.h"

@class CHHapticEngine;
@protocol CHHapticAdvancedPatternPlayer;

namespace ciface::iOS
{
class Motor : public Core::Device::Output
{
public:
  Motor(CHHapticEngine* engine, const std::string name);
  ~Motor();

  bool StartEngine();

  std::string GetName() const override;
  void SetState(ControlState state) override;

private:
  std::mutex m_lock;

  bool m_player_created = false;
  bool m_player_needs_restart = false;

  CHHapticEngine* m_haptic_engine;
  id<CHHapticAdvancedPatternPlayer> m_haptic_player;

  const std::string m_name;
  ControlState m_last_state = 0.0;
  // PrimeHack for iPad: earliest time (CFAbsoluteTime) of the next engine start attempt after a
  // failed one, so a motor whose first start failed recovers without retrying every frame.
  double m_next_start_attempt = 0.0;
};
}  // namespace ciface::iOS
