// Copyright 2020 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "InputCommon/ControllerInterface/ControllerInterface.h"

@class CHHapticEngine;

namespace ciface::iOS
{
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
}  // namespace ciface::iOS
