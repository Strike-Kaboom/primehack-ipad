// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

#import "DOLPrimeHackBridge.h"

#include <algorithm>

#include "Common/Config/Config.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/PrimeHack/HackManager.h"

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
PH_INT(aimCurve, setAimCurve, PRIMEHACK_IOS_AIM_CURVE, 0, 2)
PH_BOOL(invertX, setInvertX, PRIMEHACK_IOS_INVERT_X)
PH_BOOL(invertY, setInvertY, PRIMEHACK_IOS_INVERT_Y)
PH_BOOL(lockOnFreeAim, setLockOnFreeAim, PRIMEHACK_IOS_LOCKON_FREE_AIM)
PH_BOOL(lockCameraInMotionPuzzles, setLockCameraInMotionPuzzles, LOCKCAMERA_IN_PUZZLES)

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

@end
