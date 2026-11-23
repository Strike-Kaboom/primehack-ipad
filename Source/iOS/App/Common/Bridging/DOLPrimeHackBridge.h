// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// PrimeHack's settings (Config keys from Core/Config/MainSettings.h and GraphicsSettings.h),
/// typed for Swift. All setters write the Base layer, so they persist like any other setting.
NS_SWIFT_NAME(PrimeHackBridge)
@interface DOLPrimeHackBridge : NSObject

// PrimeHack for iPad: current emulation state.
+ (BOOL)inGameplay;
+ (BOOL)inMorphBall;

// Controls
+ (BOOL)enabled;
+ (void)setEnabled:(BOOL)value;
+ (NSInteger)stickHorizontalSensitivity;
+ (void)setStickHorizontalSensitivity:(NSInteger)value;
+ (NSInteger)stickVerticalSensitivity;
+ (void)setStickVerticalSensitivity:(NSInteger)value;
+ (BOOL)invertX;
+ (void)setInvertX:(BOOL)value;
+ (BOOL)invertY;
+ (void)setInvertY:(BOOL)value;
+ (BOOL)lockCameraInMotionPuzzles;
+ (void)setLockCameraInMotionPuzzles:(BOOL)value;

// Visuals
+ (BOOL)fovEnabled;
+ (void)setFovEnabled:(BOOL)value;
+ (NSInteger)fov;
+ (void)setFov:(NSInteger)value;
+ (BOOL)showGameCubeCrosshair;
+ (void)setShowGameCubeCrosshair:(BOOL)value;
+ (BOOL)disableBloom;
+ (void)setDisableBloom:(BOOL)value;
+ (BOOL)reduceBloom;
+ (void)setReduceBloom:(BOOL)value;
+ (NSInteger)bloomIntensity;
+ (void)setBloomIntensity:(NSInteger)value;
+ (BOOL)autoEFB;
+ (void)setAutoEFB:(BOOL)value;
+ (BOOL)armReposition;
+ (void)setArmReposition:(BOOL)value;
+ (BOOL)secondaryGunFX;
+ (void)setSecondaryGunFX:(BOOL)value;
+ (BOOL)toggleCulling;
+ (void)setToggleCulling:(BOOL)value;

// Gameplay
+ (BOOL)skippableCutscenes;
+ (void)setSkippableCutscenes:(BOOL)value;
+ (BOOL)restoreDashing;
+ (void)setRestoreDashing:(BOOL)value;
+ (BOOL)disableHudMemo;
+ (void)setDisableHudMemo:(BOOL)value;
+ (BOOL)unlockHypermode;
+ (void)setUnlockHypermode:(BOOL)value;
+ (BOOL)friendVouchers;
+ (void)setFriendVouchers:(BOOL)value;
+ (BOOL)skipPortalMP2;
+ (void)setSkipPortalMP2:(BOOL)value;
+ (BOOL)anyBeamDoors;
+ (void)setAnyBeamDoors:(BOOL)value;
+ (BOOL)invulnerability;
+ (void)setInvulnerability:(BOOL)value;
+ (BOOL)noclip;
+ (void)setNoclip:(BOOL)value;

@end

NS_ASSUME_NONNULL_END
