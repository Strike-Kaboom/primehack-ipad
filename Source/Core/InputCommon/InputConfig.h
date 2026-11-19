// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/IniFile.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/DynamicInputTextureManager.h"

namespace Common
{
class IniFile;
}

namespace ControllerEmu
{
class EmulatedController;
}

class InputConfig
{
public:
  InputConfig(std::string ini_name, std::string gui_name, std::string profile_directory_name,
              std::string profile_key);

  ~InputConfig();

  bool LoadConfig();
  void SaveConfig();
  // True while a game INI's [Controls] profile replaced at least one controller's own mapping.
  bool IsGameProfileActive() const;
  // True while a game INI's [Controls] profile replaced controller `index`'s own mapping.
  bool IsGameProfileActive(int index) const;
  // The user explicitly loaded a mapping for controller `index` (e.g. picked a profile): for the
  // rest of this game, the game INI's profile no longer applies to it and its mapping is saved.
  void SetUserOverride(int index);
  // Forget SetUserOverride choices (called when a game ends).
  void ClearUserOverrides();
  // PrimeHack for iPad: the game INI names a gamepad layout for controller `index`, but it was not
  // applied because the slot had no game controller when the config was loaded (e.g. the pad was
  // switched on after the game started). False once the user picked their own mapping.
  bool IsGameProfileSkipped(int index) const;
  // Writes the mapping that belongs on disk for `controller` into `section`: its own mapping while a
  // game's per-game profile is loaded on a game controller (with the current device), otherwise its
  // live mapping. For anything that persists a mapping (SaveConfig, the app's mapping stash).
  void SaveOwnMapping(ControllerEmu::EmulatedController* controller,
                      Common::IniFile::Section* section) const;

  template <typename T, typename... Args>
  void CreateController(Args&&... args)
  {
    m_controllers.emplace_back(std::make_unique<T>(std::forward<Args>(args)...));
  }

  ControllerEmu::EmulatedController* GetController(int index) const;
  void ClearControllers();
  bool ControllersNeedToBeCreated() const;
  bool IsControllerControlledByGamepadDevice(int index) const;

  std::string GetGUIName() const { return m_gui_name; }
  std::string GetProfileKey() const { return m_profile_key; }
  std::string GetProfileDirectoryName() const { return m_profile_directory_name; }
  std::string GetUserProfileDirectoryPath() const;
  std::string GetSysProfileDirectoryPath() const;
  int GetControllerCount() const;

  // These should be used after creating all controllers and before clearing them, respectively.
  void RegisterHotplugCallback();
  void UnregisterHotplugCallback();

  void GenerateControllerTextures(const Common::IniFile& file);
  void GenerateControllerTextures();

private:
  Common::EventHook m_hotplug_event_hook;
  std::vector<std::unique_ptr<ControllerEmu::EmulatedController>> m_controllers;
  // Per controller: its mapping comes from a game-specific profile, not its own config section.
  std::vector<bool> m_game_profile_active;
  // Per controller: the user loaded their own mapping during this game (SetUserOverride).
  std::vector<bool> m_user_override;
  // Per controller: see IsGameProfileSkipped.
  std::vector<bool> m_game_profile_skipped;
  // Per controller: its own config section from disk, kept while a per-game profile replaces it.
  std::vector<Common::IniFile::Section> m_own_sections;
  const std::string m_ini_name;
  const std::string m_gui_name;
  const std::string m_profile_directory_name;
  const std::string m_profile_key;
  InputCommon::DynamicInputTextureManager m_dynamic_input_tex_config_manager;
};
