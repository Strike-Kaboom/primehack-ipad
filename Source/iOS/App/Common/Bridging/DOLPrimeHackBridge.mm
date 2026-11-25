// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

#import "DOLPrimeHackBridge.h"

#include <algorithm>
#include <cmath>

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/iOSSettings.h"
#include "Core/PrimeHack/HackManager.h"
#include "Core/System.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#import "AudioSessionManager.h"

#define PH_BOOL(getter, setter, info)                                                              \
  +(BOOL)getter { return Config::Get(Config::info); }                                              \
  +(void)setter:(BOOL)value                                                                        \
  {                                                                                                \
    Config::SetBaseOrCurrent(Config::info, (bool)value);                                           \
    Config::Save();                                                                                \
  }

#define PH_INT(getter, setter, info, lo, hi)                                                       \
  +(NSInteger)getter { return Config::Get(Config::info); }                                         \
  +(void)setter:(NSInteger)value                                                                    \
  {                                                                                                \
    Config::SetBaseOrCurrent(Config::info, std::clamp((int)value, lo, hi));                        \
    Config::Save();                                                                                \
  }

@implementation DOLPrimeHackBridge

+ (BOOL)enabledForCurrentRun
{
  return prime::IsPrimeHackEnabledForRun();
}

+ (BOOL)inGameplay
{
  // PrimeHack for iPad: an atomic the CPU thread publishes each frame. False on the Trilogy menu,
  // the pause menu, the map and the inventory.
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateInControl) != 0;
}

+ (BOOL)inMorphBall
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateMorphBall) != 0;
}

+ (BOOL)inScanVisor
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateScanVisor) != 0;
}

+ (BOOL)hasChargeBeam
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateHasChargeBeam) != 0;
}

+ (BOOL)inHypermode
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateHypermode) != 0;
}

+ (BOOL)isLowHealth
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateLowHealth) != 0;
}

+ (BOOL)isMissileEmpty
{
  return (prime::GetPublishedPlayerState() & prime::kPlayerStateMissileEmpty) != 0;
}

+ (NSUInteger)damageCounter
{
  return static_cast<NSUInteger>(prime::GetPublishedDamageCounter());
}

+ (NSInteger)activeVisorKind
{
  const u32 state = prime::GetPublishedPlayerState();
  return static_cast<NSInteger>((state & prime::kPlayerStateVisorMask) >>
                                prime::kPlayerStateVisorShift);
}

+ (NSInteger)activeBeamKind
{
  const u32 state = prime::GetPublishedPlayerState();
  return static_cast<NSInteger>((state & prime::kPlayerStateBeamMask) >>
                                prime::kPlayerStateBeamShift);
}

// Controls
PH_BOOL(enabled, setEnabled, PRIMEHACK_ENABLE)
PH_INT(stickHorizontalSensitivity, setStickHorizontalSensitivity,
       PRIMEHACK_IOS_STICK_HORIZONTAL_SENSITIVITY, 1, 100)
PH_INT(stickVerticalSensitivity, setStickVerticalSensitivity,
       PRIMEHACK_IOS_STICK_VERTICAL_SENSITIVITY, 1, 100)
PH_INT(stickDeadzone, setStickDeadzone, PRIMEHACK_IOS_STICK_DEADZONE, 0, 30)
PH_INT(stickOuterDeadzone, setStickOuterDeadzone, PRIMEHACK_IOS_STICK_OUTER_DEADZONE, 0, 20)
PH_INT(scanVisorSensitivity, setScanVisorSensitivity, PRIMEHACK_IOS_SCAN_VISOR_SENSITIVITY, 25, 100)
PH_INT(turnAcceleration, setTurnAcceleration, PRIMEHACK_IOS_TURN_ACCEL, 0, 100)
PH_BOOL(quick180Turn, setQuick180Turn, PRIMEHACK_IOS_QUICK_180)
PH_INT(aimCurve, setAimCurve, PRIMEHACK_IOS_AIM_CURVE, 0, 2)
PH_BOOL(invertX, setInvertX, PRIMEHACK_IOS_INVERT_X)
PH_BOOL(invertY, setInvertY, PRIMEHACK_IOS_INVERT_Y)
PH_BOOL(lockOnFreeAim, setLockOnFreeAim, PRIMEHACK_IOS_LOCKON_FREE_AIM)
PH_BOOL(lockCameraInMotionPuzzles, setLockCameraInMotionPuzzles, LOCKCAMERA_IN_PUZZLES)

// Per-Game Aim Profiles
PH_BOOL(perGameAimEnabled, setPerGameAimEnabled, PRIMEHACK_IOS_PER_GAME_AIM)
PH_INT(mp1HSens, setMp1HSens, PRIMEHACK_IOS_MP1_H_SENS, 1, 100)
PH_INT(mp1VSens, setMp1VSens, PRIMEHACK_IOS_MP1_V_SENS, 1, 100)
PH_INT(mp1Deadzone, setMp1Deadzone, PRIMEHACK_IOS_MP1_DEADZONE, 0, 30)
PH_INT(mp1Fov, setMp1Fov, PRIMEHACK_IOS_MP1_FOV, 1, 170)
PH_INT(mp2HSens, setMp2HSens, PRIMEHACK_IOS_MP2_H_SENS, 1, 100)
PH_INT(mp2VSens, setMp2VSens, PRIMEHACK_IOS_MP2_V_SENS, 1, 100)
PH_INT(mp2Deadzone, setMp2Deadzone, PRIMEHACK_IOS_MP2_DEADZONE, 0, 30)
PH_INT(mp2Fov, setMp2Fov, PRIMEHACK_IOS_MP2_FOV, 1, 170)
PH_INT(mp3HSens, setMp3HSens, PRIMEHACK_IOS_MP3_H_SENS, 1, 100)
PH_INT(mp3VSens, setMp3VSens, PRIMEHACK_IOS_MP3_V_SENS, 1, 100)
PH_INT(mp3Deadzone, setMp3Deadzone, PRIMEHACK_IOS_MP3_DEADZONE, 0, 30)
PH_INT(mp3Fov, setMp3Fov, PRIMEHACK_IOS_MP3_FOV, 1, 170)

// Visuals
PH_BOOL(fovEnabled, setFovEnabled, FOV_ENABLE)
PH_INT(fov, setFov, FOV, 1, 170)
PH_BOOL(showGameCubeCrosshair, setShowGameCubeCrosshair, GC_SHOW_CROSSHAIR)
+ (NSInteger)gameCubeCrosshairColor
{
  return static_cast<NSInteger>(static_cast<u32>(Config::Get(Config::GC_CROSSHAIR_COLOR_RGBA)));
}
+ (void)setGameCubeCrosshairColor:(NSInteger)value
{
  Config::SetBaseOrCurrent(Config::GC_CROSSHAIR_COLOR_RGBA,
                           static_cast<int>(static_cast<u32>(value)));
  Config::Save();
}
PH_BOOL(disableBloom, setDisableBloom, DISABLE_BLOOM)
PH_BOOL(reduceBloom, setReduceBloom, REDUCE_BLOOM)
PH_INT(bloomIntensity, setBloomIntensity, BLOOM_INTENSITY, 0, 100)
PH_BOOL(autoEFB, setAutoEFB, AUTO_EFB)
PH_BOOL(armReposition, setArmReposition, TOGGLE_ARM_REPOSITION)
PH_INT(armPositionMode, setArmPositionMode, ARMPOSITION_MODE, 0, 1)
PH_INT(armPositionLeftRight, setArmPositionLeftRight, ARMPOSITION_LEFTRIGHT, -50, 50)
PH_INT(armPositionForwardBack, setArmPositionForwardBack, ARMPOSITION_FORWARDBACK, -50, 50)
PH_INT(armPositionUpDown, setArmPositionUpDown, ARMPOSITION_UPDOWN, -50, 50)
PH_BOOL(secondaryGunFX, setSecondaryGunFX, ENABLE_SECONDARY_GUNFX)
PH_BOOL(toggleCulling, setToggleCulling, TOGGLE_CULLING)

// Gameplay
PH_BOOL(skippableCutscenes, setSkippableCutscenes, PRIMEHACK_SKIPPABLE_CUTSCENES)
PH_BOOL(restoreDashing, setRestoreDashing, PRIMEHACK_RESTORE_SCANDASH)
PH_BOOL(disableHudMemo, setDisableHudMemo, PRIMEHACK_DISABLE_HUDMEMO)
PH_BOOL(unlockHypermode, setUnlockHypermode, PRIMEHACK_UNLOCK_HYPERMODE)
PH_BOOL(friendVouchers, setFriendVouchers, PRIMEHACK_FRIENDVOUCHERS)
PH_BOOL(skipPortalMP2, setSkipPortalMP2, PRIMEHACK_SKIPMP2_PORTAL)
PH_BOOL(anyBeamDoors, setAnyBeamDoors, PRIMEHACK_ANYBEAM_DOOR)
PH_BOOL(invulnerability, setInvulnerability, PRIMEHACK_INVULNERABILITY)
PH_BOOL(noclip, setNoclip, PRIMEHACK_NOCLIP)

// Performance
PH_BOOL(gcPrime2DualCore, setGcPrime2DualCore, PRIMEHACK_IOS_GC_MP2_DUAL_CORE)
PH_BOOL(proMotionPacing, setProMotionPacing, PRIMEHACK_IOS_PROMOTION_PACING)

// Flight Audio & Visuals
PH_BOOL(cabinDrcEnabled, setCabinDrcEnabled, MAIN_AUDIO_CABIN_DRC)

+ (BOOL)audioMixWithOthers
{
  return Config::Get(Config::MAIN_AUDIO_MIX_WITH_OTHERS);
}

+ (void)setAudioMixWithOthers:(BOOL)value
{
  Config::SetBaseOrCurrent(Config::MAIN_AUDIO_MIX_WITH_OTHERS, (bool)value);
  Config::Save();
  [[AudioSessionManager shared] setSessionCategory];
}

+ (NSInteger)darkCabinGammaLift
{
  if (!Config::Get(Config::GFX_CC_CORRECT_GAMMA))
    return 0;
  const float gamma = Config::Get(Config::GFX_CC_GAME_GAMMA);
  if (gamma >= 2.19f)
    return 0;
  const int pct = static_cast<int>(std::lround((2.2f - gamma) / 0.015f));
  return std::clamp(pct, 0, 40);
}

+ (void)setDarkCabinGammaLift:(NSInteger)value
{
  const int clamped = std::clamp((int)value, 0, 40);
  if (clamped <= 0)
  {
    Config::SetBaseOrCurrent(Config::GFX_CC_CORRECT_GAMMA, false);
    Config::SetBaseOrCurrent(Config::GFX_CC_GAME_GAMMA, 2.35f);
  }
  else
  {
    const float gamma = std::clamp(2.2f - 0.015f * static_cast<float>(clamped), 1.5f, 2.2f);
    Config::SetBaseOrCurrent(Config::GFX_CC_CORRECT_GAMMA, true);
    Config::SetBaseOrCurrent(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB, true);
    Config::SetBaseOrCurrent(Config::GFX_CC_GAME_GAMMA, gamma);
  }
  Config::Save();
}

// Live HD Texture Cache Stats
+ (NSUInteger)hiresIndexedTextureCount
{
  return static_cast<NSUInteger>(HiresTexture::GetIndexedTextureCount());
}

+ (uint64_t)hiresCacheRamUsedBytes
{
  return Core::System::GetInstance().GetCustomResourceManager().GetAssetCacheStats().ram_used;
}

+ (uint64_t)hiresCacheRamCapBytes
{
  return Core::System::GetInstance().GetCustomResourceManager().GetAssetCacheStats().ram_cap;
}

+ (uint64_t)hiresCacheActiveCount
{
  return Core::System::GetInstance().GetCustomResourceManager().GetAssetCacheStats().active_assets;
}

+ (uint64_t)hiresCachePendingCount
{
  return Core::System::GetInstance().GetCustomResourceManager().GetAssetCacheStats().pending_assets;
}

+ (uint64_t)hiresCacheEvictionCount
{
  return Core::System::GetInstance().GetCustomResourceManager().GetAssetCacheStats().evictions;
}

@end
