// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/InputConfig.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Wiimote.h"
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

  if (inifile.Load(File::GetUserPath(D_CONFIG_IDX) + m_ini_name + ".ini") &&
      !inifile.GetSections().empty())
  {
    int n = 0;
    m_game_profile_active.assign(m_controllers.size(), false);

    for (auto& controller : m_controllers)
    {
      Common::IniFile::Section config;
      // Load settings from ini
      // The slot's own mapping, including the device the app bound to it.
      const Common::IniFile::Section own_config = *inifile.GetOrCreateSection(controller->GetName());
      std::string own_device;
      own_config.Get("Device", &own_device);
      std::string keep_device;

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
      m_game_profile_active[n] = useProfile[n];

      // Next profile
      n++;
    }
    return true;
  }
  else
  {
    m_game_profile_active.assign(m_controllers.size(), false);
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

  Common::IniFile inifile;
  inifile.Load(ini_filename);

  for (size_t i = 0; i < m_controllers.size(); ++i)
  {
    auto& controller = m_controllers[i];
    auto* section = inifile.GetOrCreateSection(controller->GetName());
    if (i < m_game_profile_active.size() && m_game_profile_active[i])
    {
      // A game-specific profile (e.g. the PrimeHack layout) is loaded for this game only. Keep the
      // controller's own mapping on disk, so the next game does not inherit the per-game layout;
      // only follow a change of device (a controller reassigned mid-game).
      section->Set("Device", controller->GetDefaultDevice().ToString());
      continue;
    }
    controller->SaveConfig(section);
  }

  inifile.Save(ini_filename);
}

bool InputConfig::IsGameProfileActive() const
{
  return std::find(m_game_profile_active.begin(), m_game_profile_active.end(), true) !=
         m_game_profile_active.end();
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
