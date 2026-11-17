// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputConfig.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputProfile.h"

#include "Core/PrimeHack/HackConfig.h"

#if defined(CIFACE_USE_WIN32)
#include "Core/HotkeyManager.h"
#endif

InputConfig::InputConfig(std::string ini_name, std::string gui_name,
                         std::string profile_directory_name, std::string profile_key)
    : m_ini_name(std::move(ini_name)), m_gui_name(std::move(gui_name)),
      m_profile_directory_name(std::move(profile_directory_name)),
      m_profile_key(std::move(profile_key))
{
}

InputConfig::~InputConfig() = default;

bool InputConfig::LoadConfig()
{
  Common::IniFile inifile;
  bool useProfile[MAX_BBMOTES] = {false, false, false, false, false};
  static constexpr std::array<std::string_view, MAX_BBMOTES> num = {"1", "2", "3", "4", "BB"};
  std::string profile[MAX_BBMOTES];

  if (SConfig::GetInstance().GetGameID() != "00000000")
  {
    const std::string profile_directory = GetUserProfileDirectoryPath();

    Common::IniFile game_ini = SConfig::GetInstance().LoadGameIni();
    auto* control_section = game_ini.GetOrCreateSection("Controls");

    for (int i = 0; i < 4; i++)
    {
      const auto profile_name = fmt::format("{}Profile{}", GetProfileKey(), num[i]);

      if (control_section->Exists(profile_name))
      {
        std::string profile_setting;
        if (control_section->Get(profile_name, &profile_setting))
        {
          // PrimeHack for iPad: the Prime game INIs cover every release of a game, but PrimeHack
          // only drives the ones it supports (e.g. not the Japanese discs). Elsewhere its layout
          // would leave the right stick and pointer dead, so keep the user's own mapping.
          if (std::string_view(profile_setting).starts_with("PrimeHack") &&
              !prime::IsSupportedGameID(SConfig::GetInstance().GetGameID()))
          {
            continue;
          }

          auto profiles = InputProfile::GetProfilesFromSetting(profile_setting, profile_directory);
          // PrimeHack for iPad: game INIs in Data/Sys name profiles that ship in Data/Sys too
          // (e.g. "PrimeHack Controller"), so fall back to the bundled profile directory.
          if (profiles.empty())
          {
            profiles = InputProfile::GetProfilesFromSetting(profile_setting,
                                                            GetSysProfileDirectoryPath());
          }

          if (profiles.empty())
          {
            // TODO: PanicAlert shouldn't be used for this.
            PanicAlertFmtT("No profiles found for game setting '{0}'", profile_setting);
            continue;
          }

          // Use the first profile by default
          profile[i] = profiles[0];
          useProfile[i] = true;
        }
      }
    }
  }

  // Serialise with SaveConfig and with the host thread, which edits and saves mappings under the
  // same lock (TVControllerMappingBridge), so a save never sees a half-loaded mapping or stale
  // per-game flags. Taken after the profile lookup above, which can raise a panic alert.
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  m_user_override.resize(m_controllers.size(), false);
  std::vector<bool> profile_active(m_controllers.size(), false);
  std::vector<Common::IniFile::Section> own_sections(m_controllers.size());

  if (inifile.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini") &&
      !inifile.GetSections().empty())
  {
    int n = 0;

    for (auto& controller : m_controllers)
    {
      Common::IniFile::Section config;
      // Load settings from ini
      // The slot's own mapping, including the device the app bound to it.
      const Common::IniFile::Section own_config = *inifile.GetOrCreateSection(controller->GetName());
      std::string own_device;
      own_config.Get("Device", &own_device);
      std::string keep_device;

      // The user picked their own mapping for this slot during this game.
      if (m_user_override[n])
        useProfile[n] = false;

      if (useProfile[n])
      {
        // Load the profile from its own file: reusing `inifile` would replace the controller
        // config every later slot reads its mapping from.
        Common::IniFile profile_ini;
        profile_ini.Load(profile[n]);
        const Common::IniFile::Section profile_config = *profile_ini.GetOrCreateSection("Profile");
        std::string profile_device;
        profile_config.Get("Device", &profile_device);

        // Bundled gamepad profiles name a placeholder device ("MFi/0/Select Device"): they describe
        // a layout, not a controller. Apply one only to a slot bound to a game controller (MFi),
        // and keep that controller; a touchscreen or other slot keeps its own mapping.
        const bool placeholder = profile_device.find("Select Device") != std::string::npos;
        if (placeholder && !own_device.starts_with("MFi/"))
        {
          useProfile[n] = false;
        }
        else
        {
          std::string base;
          SplitPath(profile[n], nullptr, &base, nullptr);
          Core::DisplayMessage("Loading game specific input profile '" + base + "' for device '" +
                                   controller->GetName() + "'",
                               6000);
          config = profile_config;
          if (placeholder)
            keep_device = own_device;
        }
      }
      if (!useProfile[n])
      {
        config = own_config;
      }
      controller->LoadConfig(&config);
      if (!keep_device.empty())
        controller->SetDefaultDevice(keep_device);
      controller->UpdateReferences(g_controller_interface);
      profile_active[n] = useProfile[n];
      if (useProfile[n])
        own_sections[n] = own_config;

      // Next profile
      n++;
    }
    m_game_profile_active = std::move(profile_active);
    m_own_sections = std::move(own_sections);
    return true;
  }
  else
  {
    m_game_profile_active = std::move(profile_active);
    m_own_sections = std::move(own_sections);
    // Only load the default profile for the first controller and clear the others,
    // otherwise they would all share the same mappings on the same (default) device
    if (m_controllers.size() > 0)
    {
      m_controllers[0]->LoadDefaults(g_controller_interface);
      m_controllers[0]->UpdateReferences(g_controller_interface);
    }
    for (size_t i = 1; i < m_controllers.size(); ++i)
    {
      // Calling the base version just clears all settings without overwriting them with a default
      m_controllers[i]->EmulatedController::LoadDefaults(g_controller_interface);
      m_controllers[i]->UpdateReferences(g_controller_interface);
    }
    return false;
  }
}

void InputConfig::SaveConfig()
{
  std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
    SaveOwnMapping(controller.get(), inifile.GetOrCreateSection(controller->GetName()));

  inifile.Save(ini_filename);
}

bool InputConfig::IsGameProfileActive() const
{
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  return std::find(m_game_profile_active.begin(), m_game_profile_active.end(), true) !=
         m_game_profile_active.end();
}

bool InputConfig::IsGameProfileActive(int index) const
{
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  return index >= 0 && static_cast<size_t>(index) < m_game_profile_active.size() &&
         m_game_profile_active[index];
}

void InputConfig::SetUserOverride(int index)
{
  // Only a choice made while a game runs (or is paused, e.g. from the app's pause menu) overrides
  // that game's profile. Before boot the app loads default profiles for newly assigned
  // controllers; those must not block the per-game layout.
  const Core::State state = Core::GetState(Core::System::GetInstance());
  if (state != Core::State::Running && state != Core::State::Paused)
    return;
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  if (index < 0 || static_cast<size_t>(index) >= m_controllers.size())
    return;
  m_user_override.resize(m_controllers.size(), false);
  m_user_override[index] = true;
  if (static_cast<size_t>(index) < m_game_profile_active.size())
    m_game_profile_active[index] = false;
}

void InputConfig::SaveOwnMapping(ControllerEmu::EmulatedController* controller,
                                 Common::IniFile::Section* section) const
{
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  for (size_t i = 0; i < m_controllers.size(); ++i)
  {
    if (m_controllers[i].get() != controller)
      continue;
    // A game-specific profile (e.g. the PrimeHack layout) applies to this game only: while it is
    // loaded on a game controller, persist the controller's own mapping, following only a change
    // of game controller, so other games (and the app's mapping stash) never inherit the layout.
    // On any other device (e.g. the touchscreen the app switched to mid-game) the live mapping is
    // the user's, so it is saved as is.
    if (i < m_game_profile_active.size() && m_game_profile_active[i] &&
        i < m_own_sections.size() && controller->GetDefaultDevice().source == "MFi")
    {
      const std::string name = section->name;
      *section = m_own_sections[i];
      section->name = name;
      section->Set("Device", controller->GetDefaultDevice().ToString());
      return;
    }
    break;
  }
  controller->SaveConfig(section);
}

void InputConfig::ClearUserOverrides()
{
  const auto state_lock = ControllerEmu::EmulatedController::GetStateLock();
  m_user_override.assign(m_controllers.size(), false);
}

ControllerEmu::EmulatedController* InputConfig::GetController(int index) const
{
  return m_controllers.at(index).get();
}

void InputConfig::ClearControllers()
{
  m_controllers.clear();
}

bool InputConfig::ControllersNeedToBeCreated() const
{
  return m_controllers.empty();
}

std::string InputConfig::GetUserProfileDirectoryPath() const
{
  return fmt::format("{}Profiles/{}/", File::GetUserPath(D_CONFIG_IDX), GetProfileDirectoryName());
}

std::string InputConfig::GetSysProfileDirectoryPath() const
{
  return fmt::format("{}Profiles/{}/", File::GetSysDirectory(), GetProfileDirectoryName());
}

int InputConfig::GetControllerCount() const
{
  return static_cast<int>(m_controllers.size());
}

void InputConfig::RegisterHotplugCallback()
{
  // Update control references on all controllers
  // as configured devices may have been added or removed.
  m_hotplug_event_hook = g_controller_interface.RegisterDevicesChangedCallback([this] {
    for (auto& controller : m_controllers)
      controller->UpdateReferences(g_controller_interface);
  });
}

void InputConfig::UnregisterHotplugCallback()
{
  m_hotplug_event_hook.reset();
}

bool InputConfig::IsControllerControlledByGamepadDevice(int index) const
{
  if (static_cast<size_t>(index) >= m_controllers.size())
    return false;

  const auto& controller = m_controllers.at(index).get()->GetDefaultDevice();

  // By default on Android, no device is selected
  if (controller.source == "")
    return false;

  // Filter out anything which obviously not a gamepad
  return !((controller.source == "Quartz")      // OSX Quartz Keyboard/Mouse
           || (controller.source == "XInput2")  // Linux and BSD Keyboard/Mouse
           || (controller.source == "Android" && controller.cid <= 0)  // Android non-gamepad device
           || (controller.source == "DInput" &&
               controller.name == "Keyboard Mouse"));  // Windows Keyboard/Mouse
}

void InputConfig::GenerateControllerTextures(const Common::IniFile& file)
{
  m_dynamic_input_tex_config_manager.Load();

  std::vector<std::string> controller_names;
  for (auto& controller : m_controllers)
  {
    controller_names.push_back(controller->GetName());
  }

  m_dynamic_input_tex_config_manager.GenerateTextures(file, controller_names);
}

void InputConfig::GenerateControllerTextures()
{
  const std::string ini_filename = File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini";

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (auto& controller : m_controllers)
  {
    controller->SaveConfig(inifile.GetOrCreateSection(controller->GetName()));
  }

  GenerateControllerTextures(inifile);
}
