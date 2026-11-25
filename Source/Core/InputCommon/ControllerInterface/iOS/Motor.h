// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#ifdef __OBJC__
#include <memory>
#include <string>

#include "InputCommon/ControllerInterface/ControllerInterface.h"

@class CHHapticEngine;
#endif

namespace ciface::iOS
{
// PrimeHack for iPad: cancels any minimum-pulse hold so pause/stop/quit cuts rumble immediately.
// Exposed without Objective-C types so pure C++ callers (e.g. Core.cpp) can include this header.
void FlushRumbleImmediate();

#ifdef __OBJC__
class Motor : public Core::Device::Output
{
public:
  Motor(CHHapticEngine* engine, const std::string name);
  Motor(const Motor&) = delete;
  ~Motor();

  bool StartEngine();

  std::string GetName() const override;
  void SetState(ControlState state) override;

private:
  struct State;
  const std::shared_ptr<State> m_state;
  const std::string m_name;
};
#endif
}  // namespace ciface::iOS
