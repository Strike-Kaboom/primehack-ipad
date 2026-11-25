#include "HackConfig.h"

#include <atomic>
#include <string>
#include <mutex>

#include "Common/Config/Config.h"
#include "Common/IniFile.h"
#include "Common/FileUtil.h"
#include "Core/PrimeHack/Mods/ModHeaders.h"
#include "Core/PrimeHack/EmuVariableManager.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/GCPad.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Host.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerEmu/ControlGroup/PrimeHackAltProfile.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"

#ifdef IPHONEOS
#include <algorithm>
#include <chrono>
#include <cmath>

#include "Core/HW/GCPadEmu.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#endif

constexpr const char* PROFILES_DIR = "Profiles/";

namespace prime {
namespace {

// PrimeHack for iPad: atomic so main-thread UpdateHackSettings() (from Wiimote::LoadConfig)
// does not race with CPU-thread per-frame reads.
std::atomic<float> sensitivity{15.f};
std::atomic<float> cursor_sensitivity{15.f};

std::atomic<bool> inverted_x{false};
std::atomic<bool> inverted_y{false};
std::atomic<bool> scale_cursor_sens{false};
AddressDB addr_db;
EmuVariableManager var_mgr;
bool is_running = false;
std::atomic<CameraLock> lock_camera{CameraLock::Unlocked};
std::atomic<bool> reticle_lock{false};
std::atomic<bool> new_map_controls{false};

std::string trilogy_motd = "Thanks for using PrimeHack!\nPlease see our wiki for help!";
std::mutex motd_lock;

} // namespace

void InitializeHack() {
  if (is_running) {
    return;
  }

  is_running = true;
  PrimeMod::set_address_database(GetAddressDB());
  init_db(*GetAddressDB());
  EnableDefaultMods();
}

#ifdef IPHONEOS
namespace {
// True when the PrimeHack camera stick of this controller has no binding: the per-game
// "PrimeHack Controller" profile is not (or no longer) loaded.
bool CameraStickUnbound(ControllerEmu::ControlGroup* stick) {
  if (stick == nullptr || stick->controls.empty() || !stick->controls[0]->control_ref)
    return false;
  return stick->controls[0]->control_ref->GetExpression().empty();
}

bool BoundToGameController(InputConfig* config) {
  if (config == nullptr || config->GetControllerCount() == 0)
    return false;
  return config->GetController(0)->GetDefaultDevice().source == "MFi";
}
}  // namespace

void EnforceIOSControllerLayout(Game game) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point s_next_check{};
  static Clock::time_point s_next_reload{};
  // Consecutive checks that found the PrimeHack layout missing. The app rebinds a controller in a
  // few quick main-thread steps (set device, restore mapping, save); reloading in the middle would
  // read the config file before that save, so act only once the layout stays missing.
  static int s_unbound_checks = 0;
  const auto now = Clock::now();
  if (now < s_next_check)
    return;
  s_next_check = now + std::chrono::milliseconds(500);

  if (!IsPrimeHackEnabledForRun() || game == Game::INVALID_GAME)
    return;

  const bool is_gamecube = game >= Game::PRIME_1_GCN;
  InputConfig* config = is_gamecube ? Pad::GetConfig() : ::Wiimote::GetConfig();
  if (config == nullptr)
    return;
  bool reload = false;
  {
    // The host thread edits and saves mappings under this lock; reading them without it races.
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    // The game's PrimeHack layout was skipped when the game started because player 1 had no game
    // controller yet (the pad was switched on later). Now one is bound: reload so the layout
    // applies. Two checks in a row first, so the app has finished binding and saving it.
    static int s_skipped_checks = 0;
    if (!config->IsGameProfileActive(0) && config->IsGameProfileSkipped(0) &&
        BoundToGameController(config))
    {
      if (++s_skipped_checks >= 2 && now >= s_next_reload) {
        s_skipped_checks = 0;
        s_next_reload = now + std::chrono::seconds(10);
        reload = true;
      }
    }
    else
    {
      s_skipped_checks = 0;
    }

    // Only while the game's PrimeHack profile is in effect for player 1: not on a touchscreen
    // slot, not on a release PrimeHack does not drive, not after the user loaded their own mapping.
    if (!reload && (!config->IsGameProfileActive(0) || !BoundToGameController(config)))
      return;

    ControllerEmu::ControlGroup* stick =
        is_gamecube ? Pad::GetGroup(0, PadGroup::ControlStick) :
                      ::Wiimote::GetWiimoteGroup(0, WiimoteEmu::WiimoteGroup::ControlStick);
    if (!CameraStickUnbound(stick)) {
      s_unbound_checks = 0;
    } else if (++s_unbound_checks >= 2) {
      // Something reloaded the slot's own mapping mid-game (e.g. the app's reconnect handling).
      // Reloading the config re-applies the game's PrimeHack profile. Rate-limited so a profile
      // that cannot apply never reloads in a loop.
      s_unbound_checks = 0;
      if (now >= s_next_reload) {
        s_next_reload = now + std::chrono::seconds(10);
        reload = true;
      }
    }
    if (!is_gamecube && !CameraStickUnbound(stick)) {
      // The Wii Prime games need the Nunchuk; a reload without an "Extension =" line selects
      // None, which stops the game at "Connect the Nunchuk".
      auto* attachments = static_cast<ControllerEmu::Attachments*>(
          ::Wiimote::GetWiimoteGroup(0, WiimoteEmu::WiimoteGroup::Attachments));
      if (attachments != nullptr &&
          attachments->GetSelectedAttachment() != WiimoteEmu::ExtensionNumber::NUNCHUK) {
        attachments->SetSelectedAttachment(WiimoteEmu::ExtensionNumber::NUNCHUK);
      }
    }
  }

  // Outside the lock: LoadConfig looks up the profile first, which can raise a panic alert that
  // waits on the main thread, and only then takes the lock itself.
  if (reload) {
    if (is_gamecube) {
      Pad::LoadConfig();
      // Wiimote::LoadConfig refreshes these itself; the GC path does not.
      UpdateHackSettings();
    } else
      ::Wiimote::LoadConfig();
  }
}
#endif

void EnableDefaultMods() {
  EnableMod<SkipCutscene>(false);
  EnableMod<ViewModifier>(false);
  EnableMod<DisableBloom>(false);
  EnableMod<BloomIntensityMP3>(false);
  EnableMod<MapController>(false);
  EnableMod<STRGPatch>(false);
  EnableMod<MetareePatch>(false);
  EnableMod<AutoConvertFriendVouchers>(false);

  // Enable no PrimeHack control mods
  if (!IsPrimeHackEnabledForRun()) {
    return;
  }

  EnableMod<FpsControls>(false);
  EnableMod<SpringballButton>(false);
  EnableMod<ContextSensitiveControls>(false);
  EnableMod<ElfModLoader>(false);
}

bool CheckBeamCtl(int beam_num) {
  return Wiimote::CheckBeam(beam_num);
}

bool CheckVisorCtl(int visor_num) {
  return Wiimote::CheckVisor(visor_num);
}

bool CheckVisorScrollCtl(bool direction) {
  return Wiimote::CheckVisorScroll(direction);
}

bool CheckBeamScrollCtl(bool direction) {
  return Wiimote::CheckBeamScroll(direction);
}

bool CheckSpringBallCtl() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckSpringBall();
  } else {
    return Wiimote::CheckSpringBall();
  }
}

bool ImprovedMotionControls() {
  return Wiimote::CheckImprovedMotions();
}

bool CheckForward() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckForward();
  } else {
    return Wiimote::CheckForward();
  }
}

bool CheckBack() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckBack();
  } else {
    return Wiimote::CheckBack();
  }
}

bool CheckLeft() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckLeft();
  } else {
    return Wiimote::CheckLeft();
  }
}

bool CheckRight() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckRight();
  } else {
    return Wiimote::CheckRight();
  }
}

bool CheckJump() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::CheckJump();
  } else {
    return Wiimote::CheckJump();
  }
}

bool CheckGrappleCtl() {
  return Wiimote::CheckGrapple();
}

bool GrappleTappingMode() {
  return Wiimote::UseGrappleTapping();
}

bool GrappleCtlBound() {
  return Wiimote::GrappleCtlBound();
}

void SetEFBToTexture(bool toggle) {
  return Config::SetCurrent(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, toggle);
}

// PrimeHack for iPad: drop AutoEFB's run-time override so the user's or game INI's setting applies again.
void ClearEFBOverride() {
  if (Config::GetLayer(Config::LayerType::CurrentRun) != nullptr)
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
}

bool GetFogDisabled() {
  return Config::Get(Config::GFX_DISABLE_FOG);
}

void SetFogDisabled(bool toggle) {
  Config::SetCurrent(Config::GFX_DISABLE_FOG, toggle);
}

void ClearFogOverride() {
  // The run-time layer only exists while a game runs.
  if (Config::GetLayer(Config::LayerType::CurrentRun) != nullptr)
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::GFX_DISABLE_FOG);
}

bool GetAutoFogToggleEnabled() {
  return Config::Get(Config::AUTO_FOG_TOGGLE_MP3);
}

bool UseMPAutoEFB() {
  return Config::Get(Config::AUTO_EFB);
}

bool LockCameraInPuzzles() {
  return Config::Get(Config::LOCKCAMERA_IN_PUZZLES);
}

bool GetEFBTexture() {
  return Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
}

bool GetBloom() {
  return Config::Get(Config::DISABLE_BLOOM);
}

bool GetReduceBloom() {
  return Config::Get(Config::REDUCE_BLOOM);
}

float GetBloomIntensity() {
  return Config::Get(Config::BLOOM_INTENSITY);
}

bool GetEnableSecondaryGunFX() {
  return Config::Get(Config::ENABLE_SECONDARY_GUNFX);
}

bool GetShowGCCrosshair() {
  return Config::Get(Config::GC_SHOW_CROSSHAIR);
}

u32 GetGCCrosshairColor() {
  return Config::Get(Config::GC_CROSSHAIR_COLOR_RGBA);
}

bool GetAutoArmAdjust() {
  return Config::Get(Config::ARMPOSITION_MODE) == 0;
}

bool GetToggleArmAdjust() {
  return Config::Get(Config::TOGGLE_ARM_REPOSITION);
}

std::tuple<float, float, float> GetArmXYZ() {
  float x = Config::Get(Config::ARMPOSITION_LEFTRIGHT) / 100.f;
  float y = Config::Get(Config::ARMPOSITION_FORWARDBACK) / 100.f;
  float z = Config::Get(Config::ARMPOSITION_UPDOWN) / 100.f;

  return std::make_tuple(x, y, z);
}


std::pair<std::string, std::string> GetProfiles() {
  std::string alt_profname;
  {
    // PrimeHack for iPad: hold controller state lock while reading alt profile name on CPU thread.
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    auto* group = static_cast<ControllerEmu::PrimeHackAltProfile*>(
      Wiimote::GetWiimoteGroup(0, WiimoteEmu::WiimoteGroup::AltProfileControls));
    if (group != nullptr)
      alt_profname = group->GetAltProfileName();
  }
  std::string alt_profile_path;
  std::string main_profile_path;

  if (!alt_profname.empty() && (alt_profname != std::string("Disabled"))) {
    alt_profile_path = File::GetUserPath(D_CONFIG_IDX) + PROFILES_DIR +
                         Wiimote::GetConfig()->GetProfileKey() + "/" + alt_profname +
      ".ini";
  } else {
    alt_profile_path = "";
  }

  main_profile_path = File::GetUserPath(D_CONFIG_IDX) + WIIMOTE_INI_NAME + "_Backup.ini";

  return { alt_profile_path, main_profile_path };
}

void ChangeControllerProfileAlt(std::string profile_path) {
  Common::IniFile ini;
  ini.Load(profile_path);

  std::string profile_name = ini.GetSection("Wiimote1") ? "Wiimote1" : "Profile";

  Wiimote::GetConfig()->GetController(0)->LoadConfig(ini.GetOrCreateSection(profile_name));
  Wiimote::GetConfig()->GetController(0)->UpdateReferences(g_controller_interface);
}

void UpdateHackSettings() {
  double camera, cursor;
  bool invertx, inverty, scale_sens = false, lock = false, new_controls;

  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    std::tie<double, double, bool, bool, bool>(camera, cursor, invertx, inverty, new_controls) =
      Pad::PrimeSettings();
  } else {
    std::tie<double, double, bool, bool, bool, bool>(camera, cursor, invertx, inverty, scale_sens, lock, new_controls) =
      Wiimote::PrimeSettings();
  }

  SetSensitivity((float)camera);
  SetCursorSensitivity((float)cursor);
  SetInvertedX(invertx);
  SetInvertedY(inverty);
  SetScaleCursorSensitivity(scale_sens);
  SetReticleLock(lock);
  SetNewMapControls(new_controls);
}

float GetSensitivity() {
  return sensitivity;
}

void SetSensitivity(float sens) {
  sensitivity = sens;
}

bool HandleReticleLockOn() {
#ifdef IPHONEOS
  return Config::Get(Config::PRIMEHACK_IOS_LOCKON_FREE_AIM);
#else
  return reticle_lock;
#endif
}

#ifdef IPHONEOS
namespace {
int ActivePrimeProfileIndex(Game game) {
  switch (game) {
    case Game::PRIME_1:
    case Game::PRIME_1_GCN:
    case Game::PRIME_1_GCN_R1:
    case Game::PRIME_1_GCN_R2:
      return 1;
    case Game::PRIME_2:
    case Game::PRIME_2_GCN:
      return 2;
    case Game::PRIME_3:
    case Game::PRIME_3_STANDALONE:
      return 3;
    default:
      return 0;
  }
}
}  // namespace

std::tuple<double, double> ShapeIOSPrimeStick(double x, double y) {
  int h_sens = Config::Get(Config::PRIMEHACK_IOS_STICK_HORIZONTAL_SENSITIVITY);
  int v_sens = Config::Get(Config::PRIMEHACK_IOS_STICK_VERTICAL_SENSITIVITY);
  int deadzone_pct = Config::Get(Config::PRIMEHACK_IOS_STICK_DEADZONE);
  if (Config::Get(Config::PRIMEHACK_IOS_PER_GAME_AIM)) {
    switch (ActivePrimeProfileIndex(GetActiveGame())) {
      case 1:
        h_sens = Config::Get(Config::PRIMEHACK_IOS_MP1_H_SENS);
        v_sens = Config::Get(Config::PRIMEHACK_IOS_MP1_V_SENS);
        deadzone_pct = Config::Get(Config::PRIMEHACK_IOS_MP1_DEADZONE);
        break;
      case 2:
        h_sens = Config::Get(Config::PRIMEHACK_IOS_MP2_H_SENS);
        v_sens = Config::Get(Config::PRIMEHACK_IOS_MP2_V_SENS);
        deadzone_pct = Config::Get(Config::PRIMEHACK_IOS_MP2_DEADZONE);
        break;
      case 3:
        h_sens = Config::Get(Config::PRIMEHACK_IOS_MP3_H_SENS);
        v_sens = Config::Get(Config::PRIMEHACK_IOS_MP3_V_SENS);
        deadzone_pct = Config::Get(Config::PRIMEHACK_IOS_MP3_DEADZONE);
        break;
      default:
        break;
    }
  }
  double horizontal = static_cast<double>(std::clamp(h_sens, 1, 100));
  double vertical = static_cast<double>(std::clamp(v_sens, 1, 100));
  const double dz = static_cast<double>(std::clamp(deadzone_pct, 0, 30)) / 100.0;
  const double outer_dz =
      static_cast<double>(std::clamp(Config::Get(Config::PRIMEHACK_IOS_STICK_OUTER_DEADZONE), 0, 20)) /
      100.0;
  const double r = std::hypot(x, y);
  // Reject NaN, +/-Inf, and values inside the deadzone so a bad reading cannot poison pitch.
  if (!std::isfinite(r) || r <= dz || r <= 1e-6)
    return {0.0, 0.0};

  const double span = std::max(0.05, 1.0 - dz - outer_dz);
  const double norm = std::min(1.0, (r - dz) / span);
  const int curve = Config::Get(Config::PRIMEHACK_IOS_AIM_CURVE);
  double shaped = norm;
  if (curve == 1)
    shaped = std::pow(norm, 1.6);
  else if (curve == 2)
    shaped = std::pow(norm, 2.2);

  // Finer aim while actively scanning in first-person (Scan Visor sensitivity scale).
  const u32 pstate = GetPublishedPlayerState();
  if ((pstate & kPlayerStateInControl) != 0 && (pstate & kPlayerStateScanVisor) != 0 &&
      (pstate & kPlayerStateMorphBall) == 0) {
    const double scan_scale =
        static_cast<double>(std::clamp(Config::Get(Config::PRIMEHACK_IOS_SCAN_VISOR_SENSITIVITY), 25, 100)) /
        100.0;
    horizontal *= scan_scale;
    vertical *= scan_scale;
  }

  const double scale = shaped / r;
  return {x * scale * horizontal, y * scale * -vertical};
}
#endif

bool NewMapControlsEnabled() {
  return new_map_controls;
}

void SetNewMapControls(bool new_controls) {
  new_map_controls = new_controls;
}

void SetReticleLock(bool lock) {
  reticle_lock = lock;
}

float GetCursorSensitivity() {
  return cursor_sensitivity;
}

void SetCursorSensitivity(float sens) {
  cursor_sensitivity = sens;
}

// Restore default FOV based on config
float GetFov(Game game) {
  if (Config::Get(Config::FOV_ENABLE)) {
#ifdef IPHONEOS
    if (Config::Get(Config::PRIMEHACK_IOS_PER_GAME_AIM)) {
      switch (ActivePrimeProfileIndex(game)) {
        case 1: return static_cast<float>(std::clamp(Config::Get(Config::PRIMEHACK_IOS_MP1_FOV), 1, 170));
        case 2: return static_cast<float>(std::clamp(Config::Get(Config::PRIMEHACK_IOS_MP2_FOV), 1, 170));
        case 3: return static_cast<float>(std::clamp(Config::Get(Config::PRIMEHACK_IOS_MP3_FOV), 1, 170));
        default: break;
      }
    }
#endif
    return static_cast<float>(std::clamp(Config::Get(Config::FOV), 1, 170));
  } else {
    switch (game) {
      case Game::PRIME_1:
      case Game::PRIME_1_GCN:
      case Game::PRIME_1_GCN_R1:
      case Game::PRIME_1_GCN_R2:
        return 55.f;
      default:
        return 60.f;
      }
  }
}

bool InvertedY() {
#ifdef IPHONEOS
  // PrimeHack for iPad: read the settings screen's value live, so a change made mid-game applies at
  // once (the cached copy only refreshes when the game or controller config reloads).
  return Config::Get(Config::PRIMEHACK_IOS_INVERT_Y);
#else
  return inverted_y;
#endif
}

void SetInvertedY(bool inverted) {
  inverted_y = inverted;
}

bool InvertedX() {
#ifdef IPHONEOS
  return Config::Get(Config::PRIMEHACK_IOS_INVERT_X);
#else
  return inverted_x;
#endif
}

void SetInvertedX(bool inverted) {
  inverted_x = inverted;
}

bool ScaleCursorSensitivity() {
  return scale_cursor_sens;
}

void SetScaleCursorSensitivity(bool scale) {
  scale_cursor_sens = scale;
}

bool CheckPitchRecentre() {
  if (ControllerMode()) {
    if (GetActiveGame() >= Game::PRIME_1_GCN) {
      return Pad::CheckPitchRecentre();
    } else {
      return Wiimote::CheckPitchRecentre();
    }
  }

  return false;
}

bool CheckQuickTurn() {
#ifdef IPHONEOS
  if (!Config::Get(Config::PRIMEHACK_IOS_QUICK_180)) {
    return false;
  }
#endif
  if (ControllerMode()) {
    if (GetActiveGame() >= Game::PRIME_1_GCN) {
      return Pad::CheckQuickTurn();
    } else {
      return Wiimote::CheckQuickTurn();
    }
  }

  return false;
}

bool ControllerMode() {
  if (GetActiveGame() == Game::INVALID_GAME) {
    return true;
  }

  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    return Pad::PrimeUseController();
  } else {
    return Wiimote::PrimeUseController();
  }
}

double GetHorizontalAxis() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    if (Pad::PrimeUseController()) {
      return std::get<0>(Pad::GetPrimeStickXY());
    }
  } else if (Wiimote::PrimeUseController()) {
    return std::get<0>(Wiimote::GetPrimeStickXY());
  }

  if (!Host_RendererHasFocus() || !g_mouse_input) {
    return 0;
  }

  return static_cast<double>(g_mouse_input->GetDeltaHorizontalAxis());
}

double GetVerticalAxis() {
  if (GetActiveGame() >= Game::PRIME_1_GCN) {
    if (Pad::PrimeUseController()) {
      return std::get<1>(Pad::GetPrimeStickXY());
    }
  } else if (Wiimote::PrimeUseController()) {
    return std::get<1>(Wiimote::GetPrimeStickXY());
  }

  if (!Host_RendererHasFocus() || !g_mouse_input) {
    return 0;
  }

  return static_cast<double>(g_mouse_input->GetDeltaVerticalAxis());
}

bool GetCulling() {
  return Config::Get(Config::TOGGLE_CULLING);
}

void SetLockCamera(CameraLock lock) {
  lock_camera = lock;
}

CameraLock GetLockCamera() {
  return lock_camera;
}

std::tuple<bool, bool> GetMenuOptions() {
  return Wiimote::GetBVMenuOptions();
}

AddressDB* GetAddressDB() {
  return &addr_db;
}

EmuVariableManager* GetVariableManager() {
  return &var_mgr;
}

void SetMotd(std::string const& motd) {
  auto lock = std::lock_guard<std::mutex>(motd_lock);
  trilogy_motd = motd;
}

std::string GetMotd() {
  auto lock = std::lock_guard<std::mutex>(motd_lock);
  return trilogy_motd;
}

bool UsingRealWiimote() {
  return Wiimote::GetSource(0) == WiimoteSource::Real;
}

}  // namespace prime
