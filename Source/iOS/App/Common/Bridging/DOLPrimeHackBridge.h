// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// PrimeHack's settings (Config keys from Core/Config/MainSettings.h and GraphicsSettings.h),
/// typed for Swift. All setters write the Base layer, so they persist like any other setting.
NS_SWIFT_NAME(PrimeHackBridge)
@interface DOLPrimeHackBridge : NSObject

// PrimeHack for iPad: current emulation state.
+ (BOOL)enabledForCurrentRun;
+ (BOOL)inGameplay;
+ (BOOL)inMorphBall;
+ (BOOL)inScanVisor;
+ (BOOL)hasChargeBeam;
+ (BOOL)inHypermode;
+ (BOOL)isLowHealth;
+ (BOOL)isMissileEmpty;
+ (NSUInteger)damageCounter;
+ (NSInteger)activeVisorKind;
+ (NSInteger)activeBeamKind;

// Controls
+ (BOOL)enabled;
+ (void)setEnabled:(BOOL)value;
+ (NSInteger)stickHorizontalSensitivity;
+ (void)setStickHorizontalSensitivity:(NSInteger)value;
+ (NSInteger)stickVerticalSensitivity;
+ (void)setStickVerticalSensitivity:(NSInteger)value;
+ (NSInteger)stickDeadzone;
+ (void)setStickDeadzone:(NSInteger)value;
+ (NSInteger)stickOuterDeadzone;
+ (void)setStickOuterDeadzone:(NSInteger)value;
+ (NSInteger)scanVisorSensitivity;
+ (void)setScanVisorSensitivity:(NSInteger)value;
+ (NSInteger)turnAcceleration;
+ (void)setTurnAcceleration:(NSInteger)value;
+ (BOOL)quick180Turn;
+ (void)setQuick180Turn:(BOOL)value;
+ (NSInteger)aimCurve;
+ (void)setAimCurve:(NSInteger)value;
+ (BOOL)invertX;
+ (void)setInvertX:(BOOL)value;
+ (BOOL)invertY;
+ (void)setInvertY:(BOOL)value;
+ (BOOL)lockOnFreeAim;
+ (void)setLockOnFreeAim:(BOOL)value;
+ (BOOL)lockCameraInMotionPuzzles;
+ (void)setLockCameraInMotionPuzzles:(BOOL)value;

// Per-Game Aim Profiles (Prime 1 / Prime 2 / Prime 3)
+ (BOOL)perGameAimEnabled;
+ (void)setPerGameAimEnabled:(BOOL)value;
+ (NSInteger)mp1HSens;
+ (void)setMp1HSens:(NSInteger)value;
+ (NSInteger)mp1VSens;
+ (void)setMp1VSens:(NSInteger)value;
+ (NSInteger)mp1Deadzone;
+ (void)setMp1Deadzone:(NSInteger)value;
+ (NSInteger)mp1Fov;
+ (void)setMp1Fov:(NSInteger)value;
+ (NSInteger)mp2HSens;
+ (void)setMp2HSens:(NSInteger)value;
+ (NSInteger)mp2VSens;
+ (void)setMp2VSens:(NSInteger)value;
+ (NSInteger)mp2Deadzone;
+ (void)setMp2Deadzone:(NSInteger)value;
+ (NSInteger)mp2Fov;
+ (void)setMp2Fov:(NSInteger)value;
+ (NSInteger)mp3HSens;
+ (void)setMp3HSens:(NSInteger)value;
+ (NSInteger)mp3VSens;
+ (void)setMp3VSens:(NSInteger)value;
+ (NSInteger)mp3Deadzone;
+ (void)setMp3Deadzone:(NSInteger)value;
+ (NSInteger)mp3Fov;
+ (void)setMp3Fov:(NSInteger)value;

// Visuals
+ (BOOL)fovEnabled;
+ (void)setFovEnabled:(BOOL)value;
+ (NSInteger)fov;
+ (void)setFov:(NSInteger)value;
+ (BOOL)showGameCubeCrosshair;
+ (void)setShowGameCubeCrosshair:(BOOL)value;
+ (NSInteger)gameCubeCrosshairColor;
+ (void)setGameCubeCrosshairColor:(NSInteger)value;
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
+ (NSInteger)armPositionMode;
+ (void)setArmPositionMode:(NSInteger)value;
+ (NSInteger)armPositionLeftRight;
+ (void)setArmPositionLeftRight:(NSInteger)value;
+ (NSInteger)armPositionForwardBack;
+ (void)setArmPositionForwardBack:(NSInteger)value;
+ (NSInteger)armPositionUpDown;
+ (void)setArmPositionUpDown:(NSInteger)value;
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

// Performance
/// Opt-in: GameCube Prime 2 boots dual-core with SyncGPU (as Trilogy's Prime 2 does) instead of
/// G2M.ini's single core. Read once when a game boots.
+ (BOOL)gcPrime2DualCore;
+ (void)setGcPrime2DualCore:(BOOL)value;
/// Opt-in: pace Metal presents on 120 Hz ProMotion to the game's refresh rate.
+ (BOOL)proMotionPacing;
+ (void)setProMotionPacing:(BOOL)value;

// Flight Audio & Visuals
+ (BOOL)cabinDrcEnabled;
+ (void)setCabinDrcEnabled:(BOOL)value;
+ (BOOL)audioMixWithOthers;
+ (void)setAudioMixWithOthers:(BOOL)value;
/// Dark-cabin gamma lift (0 = off, 5..40 = shadow lift % via Dolphin's ColorCorrection gamma).
+ (NSInteger)darkCabinGammaLift;
+ (void)setDarkCabinGammaLift:(NSInteger)value;

// Live HD Texture Cache Stats (lock-free, safe from main thread)
+ (NSUInteger)hiresIndexedTextureCount;
+ (uint64_t)hiresCacheRamUsedBytes;
+ (uint64_t)hiresCacheRamCapBytes;
+ (uint64_t)hiresCacheActiveCount;
+ (uint64_t)hiresCachePendingCount;
+ (uint64_t)hiresCacheEvictionCount;

@end

NS_ASSUME_NONNULL_END
