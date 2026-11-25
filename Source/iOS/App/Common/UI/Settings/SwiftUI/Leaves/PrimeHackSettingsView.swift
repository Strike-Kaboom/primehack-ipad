// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GameController
import SwiftUI

/// PrimeHack for iPad: when this sideloaded copy stops opening. A sideloaded app carries its
/// provisioning profile, and once that expires iPadOS refuses to launch the app until it is signed
/// again, which needs a computer. Worth knowing before a long trip.
enum AppSigningExpiry {
  struct Validity: Equatable {
    let created: Date?
    let expires: Date
  }

  /// The embedded profile's dates, or nil when the build carries none (Simulator, a store build).
  static func validity(bundle: Bundle = .main) -> Validity? {
    guard let url = bundle.url(forResource: "embedded", withExtension: "mobileprovision"),
          let data = try? Data(contentsOf: url),
          // The profile is a CMS envelope around an XML property list: cut the list out by its markers.
          let start = data.range(of: Data("<?xml".utf8)),
          let end = data.range(of: Data("</plist>".utf8), in: start.lowerBound ..< data.endIndex),
          let object = try? PropertyListSerialization.propertyList(
            from: data.subdata(in: start.lowerBound ..< end.upperBound), options: [], format: nil),
          let plist = object as? [String: Any],
          let expires = plist["ExpirationDate"] as? Date
    else { return nil }
    return Validity(created: plist["CreationDate"] as? Date, expires: expires)
  }

  /// Whole days until `expires` (negative once past).
  static func daysRemaining(_ validity: Validity, now: Date = Date()) -> Int {
    Int((validity.expires.timeIntervalSince(now) / 86_400).rounded(.down))
  }

  /// Days of notice worth a reminder: two weeks for a yearly signature, two days for a weekly one.
  static func warningDays(_ validity: Validity) -> Int {
    guard let created = validity.created else { return 14 }
    return validity.expires.timeIntervalSince(created) > 30 * 86_400 ? 14 : 2
  }

  /// The Settings caption, or nil when the build carries no profile.
  static func caption(now: Date = Date()) -> String? {
    guard let signed = AppSigningExpiry.validity() else { return nil }
    let days = daysRemaining(signed, now: now)
    let date = signed.expires.formatted(date: .abbreviated, time: .omitted)
    let clamped = max(days, 0)
    let remaining = clamped == 0
      ? L("expires today")
      : clamped == 1
        ? L("1 day left")
        : String(format: L("%d days left"), clamped)
    if days <= warningDays(signed) {
      return String(format: L("This copy of PrimeHack is signed until %@ (%@). Re-install it from your computer before then, and before any long trip: once the signature expires the app will not open."), date, remaining)
    }
    return String(format: L("This copy of PrimeHack is signed until %@ (%@). After that it has to be re-installed from your computer."), date, remaining)
  }
}

/// PrimeHack for iPad: the two "playing unattended" protections (Settings > PrimeHack > On a Flight).
/// Stored in UserDefaults like the other app-side PrimeHack options; both are in Reset All Settings.
enum PrimeHackFlightOptions {
  static let pauseOnHeadphonesOutKey = "primehack_pause_on_headphones_out"
  static let idlePauseMinutesKey = "primehack_idle_pause_minutes"
  static let pauseAfterSleepKey = "primehack_pause_after_sleep"
  static let thermalResStepDownKey = "primehack_thermal_res_stepdown"
  /// Generous on purpose, so watching a long cutscene or credits roll is unlikely to trip it.
  static let defaultIdlePauseMinutes = 15
  /// 0 = Off.
  static let idlePauseMinuteChoices = [0, 5, 10, 15, 30]

  static func pauseOnHeadphonesOut(in defaults: UserDefaults = .standard) -> Bool {
    defaults.object(forKey: pauseOnHeadphonesOutKey) as? Bool ?? true
  }

  static func setPauseOnHeadphonesOut(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: pauseOnHeadphonesOutKey)
  }

  /// Minutes without controller input before the game pauses and stops holding the screen awake.
  static func idlePauseMinutes(in defaults: UserDefaults = .standard) -> Int {
    let stored = defaults.object(forKey: idlePauseMinutesKey) as? Int ?? defaultIdlePauseMinutes
    return idlePauseMinuteChoices.contains(stored) ? stored : defaultIdlePauseMinutes
  }

  static func setIdlePauseMinutes(_ minutes: Int, in defaults: UserDefaults = .standard) {
    defaults.set(minutes, forKey: idlePauseMinutesKey)
  }

  /// Land on the Pause Menu after sleep/backgrounding and wait for a button press on reconnect.
  static func pauseAfterSleep(in defaults: UserDefaults = .standard) -> Bool {
    #if os(iOS)
    defaults.object(forKey: pauseAfterSleepKey) as? Bool ?? true
    #else
    false
    #endif
  }

  static func setPauseAfterSleep(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: pauseAfterSleepKey)
  }

  /// Drop internal resolution by 1 step on `.serious`/`.critical` thermal state, restore when cool.
  static func thermalResStepDown(in defaults: UserDefaults = .standard) -> Bool {
    defaults.object(forKey: thermalResStepDownKey) as? Bool ?? true
  }

  static func setThermalResStepDown(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: thermalResStepDownKey)
  }
}

/// Snapshot of PrimeHack's settings. Defaults match the core's Config defaults.
struct PrimeHackSettingsState: Equatable {
  var enabled = true
  var controllerOnly = true
  var adaptiveTriggers = true
  var triggerStrength = 1
  var dualSenseLightbar = true
  var lightbarBrightness = 1
  var damageMissileCues = true
  var rumbleStrength = 100.0
  var rumbleMinPulseMs = 50
  var horizontalSensitivity = 45.0
  var verticalSensitivity = 35.0
  var stickDeadzone = 10.0
  var stickOuterDeadzone = 0.0
  var scanVisorSensitivity = 100.0
  var turnAcceleration = 0.0
  var quick180Turn = true
  var aimCurve = 0
  var invertX = false
  var invertY = false
  var lockOnFreeAim = false
  var lockCameraInPuzzles = true
  var perGameAimEnabled = false

  var fovEnabled = false
  var fov = 60.0
  var darkCabinGammaLift = 0.0
  var gcCrosshair = false
  var gcCrosshairColor = 0x4b7ea331
  var disableBloom = false
  var reduceBloom = true
  var bloomIntensity = 65.0
  var autoEFB = true
  var armReposition = true
  var armPositionMode = 0
  var armPositionLeftRight = 25.0
  var armPositionForwardBack = -35.0
  var armPositionUpDown = -30.0
  var secondaryGunFX = false
  var toggleCulling = false

  var skippableCutscenes = false
  var restoreDashing = false
  var disableHudMemo = false
  var unlockHypermode = true
  var friendVouchers = false
  var skipPortalMP2 = false
  var anyBeamDoors = false
  var invulnerability = false
  var noclip = false

  var gcPrime2DualCore = false
  var proMotionPacing = false

  var pauseOnHeadphonesOut = true
  var pauseAfterSleep = true
  var idlePauseMinutes = PrimeHackFlightOptions.defaultIdlePauseMinutes
  var thermalResStepDown = true
  var cabinDrcEnabled = false
  var audioMixWithOthers = false

  /// "Signed until …" for a sideloaded copy; nil when the build carries no provisioning profile.
  var signingCaption: String? = nil

  static let sensitivityRange = 1.0 ... 100.0
  static let deadzoneRange = 0.0 ... 30.0
  static let outerDeadzoneRange = 0.0 ... 20.0
  static let scanVisorSensitivityRange = 25.0 ... 100.0
  static let turnAccelRange = 0.0 ... 100.0
  static let darkCabinGammaRange = 0.0 ... 40.0
  static let rumbleStrengthRange = 25.0 ... 100.0
  static let armOffsetRange = -50.0 ... 50.0
  static let fovRange = 1.0 ... 170.0
  static let bloomIntensityRange = 0.0 ... 100.0
  static let defaultGCCrosshairColor = 0x4b7ea331
  static let rumbleMinPulseOptions: [(String, Int)] = [
    (L("Off"), 0),
    ("40 ms", 40),
    ("50 ms", 50),
    ("60 ms", 60),
  ]
  static let triggerStrengthOptions: [(String, Int)] = [
    (L("Light"), 0),
    (L("Medium"), 1),
    (L("Strong"), 2),
  ]
  static let lightbarBrightnessOptions: [(String, Int)] = [
    (L("Subtle"), 0),
    (L("Medium"), 1),
    (L("Bright"), 2),
  ]
  static let aimCurveOptions: [(String, Int)] = [
    (L("Linear"), 0),
    (L("Smooth"), 1),
    (L("Precision"), 2),
  ]
  static let armPositionModeOptions: [(String, Int)] = [
    (L("Auto"), 0),
    (L("Manual"), 1),
  ]
  static let idlePauseOptions: [(String, Int)] = [
    (L("Off"), 0),
    (L("5 minutes"), 5),
    (L("10 minutes"), 10),
    (L("15 minutes"), 15),
    (L("30 minutes"), 30),
  ]
  static let gcCrosshairColorOptions: [(String, Int)] = [
    (L("Default (Blue)"), 0x4b7ea331),
    (L("Cyan"), 0x38d9ffc8),
    (L("Green"), 0x40e870c8),
    (L("Amber"), 0xffb830c8),
    (L("Red"), 0xff4545c8),
    (L("White"), 0xffffffc8),
  ]
}

/// One user edit. The view applies it to its snapshot AND to Config; the builder only emits it.
enum PrimeHackSettingsChange: Equatable {
  case enabled(Bool)
  case controllerOnly(Bool)
  case adaptiveTriggers(Bool)
  case triggerStrength(Int)
  case dualSenseLightbar(Bool)
  case lightbarBrightness(Int)
  case damageMissileCues(Bool)
  case rumbleStrength(Double)
  case rumbleMinPulseMs(Int)
  case horizontalSensitivity(Double)
  case verticalSensitivity(Double)
  case stickDeadzone(Double)
  case stickOuterDeadzone(Double)
  case scanVisorSensitivity(Double)
  case turnAcceleration(Double)
  case quick180Turn(Bool)
  case aimCurve(Int)
  case invertX(Bool)
  case invertY(Bool)
  case lockOnFreeAim(Bool)
  case lockCameraInPuzzles(Bool)
  case fovEnabled(Bool)
  case fov(Double)
  case darkCabinGammaLift(Double)
  case gcCrosshair(Bool)
  case gcCrosshairColor(Int)
  case disableBloom(Bool)
  case reduceBloom(Bool)
  case bloomIntensity(Double)
  case autoEFB(Bool)
  case armReposition(Bool)
  case armPositionMode(Int)
  case armPositionLeftRight(Double)
  case armPositionForwardBack(Double)
  case armPositionUpDown(Double)
  case secondaryGunFX(Bool)
  case toggleCulling(Bool)
  case skippableCutscenes(Bool)
  case restoreDashing(Bool)
  case disableHudMemo(Bool)
  case unlockHypermode(Bool)
  case friendVouchers(Bool)
  case skipPortalMP2(Bool)
  case anyBeamDoors(Bool)
  case invulnerability(Bool)
  case noclip(Bool)
  case gcPrime2DualCore(Bool)
  case proMotionPacing(Bool)
  case pauseOnHeadphonesOut(Bool)
  case pauseAfterSleep(Bool)
  case idlePauseMinutes(Int)
  case thermalResStepDown(Bool)
  case cabinDrcEnabled(Bool)
  case audioMixWithOthers(Bool)
}

/// Pure: snapshot in, changes out.
enum PrimeHackSettingsModelBuilder {
  static func make(state s: PrimeHackSettingsState,
                   jitSection: MenuSection? = nil,
                   apply: @escaping (PrimeHackSettingsChange) -> Void) -> MenuModel {
    let whole: (Double) -> String = { String(Int($0.rounded())) }
    let degrees: (Double) -> String = { "\(Int($0.rounded()))°" }
    let percent: (Double) -> String = { "\(Int($0.rounded()))%" }

    let controls = MenuSection(id: "primehack-controls", header: L("Controls"),
                               footer: L("Applies to Metroid Prime, Prime 2 and Trilogy when a game controller is on player 1. Left stick moves, right stick looks. D-Pad picks a beam. L1 + D-Pad picks a visor: Up Combat, Left Scan, Down Thermal/Dark, Right X-Ray/Echo. R2 fires, Cross jumps, Square morphs, Triangle fires a missile, L2 locks on, R1 opens the map. Press R3 in GameCube (or L1 + R3 in Trilogy) to centre camera pitch."),
                               items: [
      SettingsRow.toggle("primehack-enabled", L("PrimeHack Controls"), s.enabled,
                         L("FPS-style camera control and quick beam and visor selection in the Metroid Prime games. Takes effect when a game starts."),
                         defaultValue: true, set: { apply(.enabled($0)) }),
      SettingsRow.toggle("primehack-controller-only", L("Controller Only"), s.controllerOnly,
                         L("Your game controller is always Player 1, in every game, and the on-screen controls are never used. Takes effect when a game starts or a controller connects."),
                         defaultValue: true, set: { apply(.controllerOnly($0)) }),
      SettingsRow.toggle("primehack-adaptive-triggers", L("Adaptive Triggers (DualSense)"), s.adaptiveTriggers,
                         L("R2 clicks like a trigger tuned to your active beam and hums as a charge builds; L2 pushes back lightly for lock-on. Only in the Metroid Prime games, never in menus."),
                         defaultValue: true, set: { apply(.adaptiveTriggers($0)) }),
      SettingsRow.picker("primehack-trigger-strength", L("Trigger Strength"),
                         PrimeHackSettingsState.triggerStrengthOptions, s.triggerStrength,
                         L("How firm the DualSense adaptive trigger resistance and charge vibration feel."),
                         enabled: s.adaptiveTriggers,
                         defaultValue: 1,
                         disabledReason: L("Enable Adaptive Triggers (DualSense) first."),
                         set: { apply(.triggerStrength($0)) }),
      SettingsRow.toggle("primehack-dualsense-lightbar", L("DualSense Lightbar"), s.dualSenseLightbar,
                         L("Softly tints the DualSense lightbar for your active beam, visor, Morph Ball, Hypermode, and low energy, and restores it in menus or when paused."),
                         defaultValue: true, set: { apply(.dualSenseLightbar($0)) }),
      SettingsRow.picker("primehack-lightbar-brightness", L("Lightbar Brightness"),
                         PrimeHackSettingsState.lightbarBrightnessOptions, s.lightbarBrightness,
                         L("Overall brightness of the DualSense lightbar during gameplay. Subtle and Medium are tuned for dark cabins."),
                         enabled: s.dualSenseLightbar,
                         defaultValue: 1,
                         disabledReason: L("Enable DualSense Lightbar first."),
                         set: { apply(.lightbarBrightness($0)) }),
      SettingsRow.toggle("primehack-damage-missile-cues", L("Damage & Missile Haptic Cues"), s.damageMissileCues,
                         L("Adds a crisp DualSense haptic thump when Samus takes damage, and a dry click when pressing Missile while out of missiles (Prime 1)."),
                         defaultValue: true, set: { apply(.damageMissileCues($0)) }),
      SettingsRow.stepper("primehack-rumble-strength", L("Rumble Strength"), s.rumbleStrength,
                          range: PrimeHackSettingsState.rumbleStrengthRange, step: 5, format: percent,
                          L("Scales DualSense and device vibration intensity live without reconnecting the controller."),
                          defaultValue: 100, set: { apply(.rumbleStrength($0)) }),
      SettingsRow.picker("primehack-rumble-min-pulse", L("Minimum Rumble Pulse"),
                         PrimeHackSettingsState.rumbleMinPulseOptions, s.rumbleMinPulseMs,
                         L("Holds very short 1-frame GameCube rumble pulses slightly longer so Bluetooth haptics never swallow them. Pause and quit still cut rumble immediately."),
                         defaultValue: 50, set: { apply(.rumbleMinPulseMs($0)) }),
      SettingsRow.stepper("primehack-h-sens", L("Look Sensitivity (Horizontal)"), s.horizontalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick turns the camera left and right."),
                          enabled: s.enabled, defaultValue: 45, set: { apply(.horizontalSensitivity($0)) }),
      SettingsRow.stepper("primehack-v-sens", L("Look Sensitivity (Vertical)"), s.verticalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick tilts the camera up and down."),
                          enabled: s.enabled, defaultValue: 35, set: { apply(.verticalSensitivity($0)) }),
      SettingsRow.stepper("primehack-scan-sens", L("Scan Visor Sensitivity"), s.scanVisorSensitivity,
                          range: PrimeHackSettingsState.scanVisorSensitivityRange, step: 5, format: percent,
                          L("Scales right-stick look speed while in Scan Visor for finer aim on small scan points."),
                          enabled: s.enabled, defaultValue: 100, set: { apply(.scanVisorSensitivity($0)) }),
      SettingsRow.stepper("primehack-deadzone", L("Right Stick Deadzone"), s.stickDeadzone,
                          range: PrimeHackSettingsState.deadzoneRange, step: 2, format: percent,
                          L("Ignores small stick deflections around centre to eliminate DualSense stick drift."),
                          enabled: s.enabled, defaultValue: 10, set: { apply(.stickDeadzone($0)) }),
      SettingsRow.stepper("primehack-outer-deadzone", L("Right Stick Outer Deadzone"), s.stickOuterDeadzone,
                          range: PrimeHackSettingsState.outerDeadzoneRange, step: 2, format: percent,
                          L("Reaches 100% turn speed slightly before the physical rim, compensating for worn analog sticks."),
                          enabled: s.enabled, defaultValue: 0, set: { apply(.stickOuterDeadzone($0)) }),
      SettingsRow.stepper("primehack-turn-accel", L("Turn Acceleration Ramp"), s.turnAcceleration,
                          range: PrimeHackSettingsState.turnAccelRange, step: 10, format: percent,
                          L("Smoothly boosts turn speed when holding the right stick near full tilt, for quick about-faces without raising base sensitivity. 0% = off."),
                          enabled: s.enabled, defaultValue: 0, set: { apply(.turnAcceleration($0)) }),
      SettingsRow.picker("primehack-aim-curve", L("Aim Response Curve"),
                         PrimeHackSettingsState.aimCurveOptions, s.aimCurve,
                         L("Linear turns evenly with stick tilt; Smooth and Precision soften small tilts for finer aiming while keeping full turn speed at the edge."),
                         enabled: s.enabled, defaultValue: 0, set: { apply(.aimCurve($0)) }),
      SettingsRow.toggle("primehack-quick-180", L("Quick 180° Turn (L1 + L3)"), s.quick180Turn,
                         L("Press L1 + L3 in first-person to smoothly spin 180° around."),
                         enabled: s.enabled, defaultValue: true, set: { apply(.quick180Turn($0)) }),
      SettingsRow.toggle("primehack-invert-x", L("Invert Horizontal Look"), s.invertX,
                         L("Pushing the right stick right turns the camera left."),
                         enabled: s.enabled, defaultValue: false, set: { apply(.invertX($0)) }),
      SettingsRow.toggle("primehack-invert-y", L("Invert Vertical Look"), s.invertY,
                         L("Pushing the right stick up looks down, like a flight stick."),
                         enabled: s.enabled, defaultValue: false, set: { apply(.invertY($0)) }),
      SettingsRow.toggle("primehack-lockon-free-aim", L("Free Aim While Locked-On (Trilogy)"), s.lockOnFreeAim,
                         L("Lets you steer the aiming reticle while holding L2 lock-on in Metroid Prime Trilogy."),
                         enabled: s.enabled, defaultValue: false, set: { apply(.lockOnFreeAim($0)) }),
      SettingsRow.toggle("primehack-lock-puzzles", L("Lock Camera in Motion Puzzles"), s.lockCameraInPuzzles,
                         L("Holds the camera still while you work a Trilogy motion puzzle, such as turning a lock."),
                         enabled: s.enabled, defaultValue: true, set: { apply(.lockCameraInPuzzles($0)) }),
      SettingsRow.destination("primehack-per-game-aim", L("Per-Game Aim Profiles"),
                              s.perGameAimEnabled
                                ? L("Active: Prime 1, Prime 2 and Prime 3 each use their own sensitivity, deadzone and FOV.")
                                : L("Optional independent sensitivity, deadzone and FOV for Prime 1, Prime 2 and Prime 3."),
                              icon: "slider.horizontal.3",
                              view: AnyView(PrimeHackPerGameAimView())),
      SettingsRow.destination("primehack-controls-card-row", L("DualSense Controls Card"),
                              L("Quick reference for beams, L1 + D-Pad visors, Quick 180° and camera pitch reset."),
                              icon: "gamecontroller",
                              view: AnyView(PrimeHackControlsCardView())),
    ])

    var visualItems: [MenuItem] = [
      SettingsRow.toggle("primehack-fov-enabled", L("Custom Field of View"), s.fovEnabled,
                         L("Replaces the game's field of view with the value below."),
                         defaultValue: false, set: { apply(.fovEnabled($0)) }),
      SettingsRow.stepper("primehack-fov", L("Field of View"), s.fov,
                          range: PrimeHackSettingsState.fovRange, step: 5, format: degrees,
                          L("Horizontal field of view in degrees. Around 75 to 90 suits a big screen. Above 100 (Prime 3: above 94) the game also draws objects outside the view, which costs speed."),
                          enabled: s.fovEnabled, defaultValue: 60, set: { apply(.fov($0)) }),
      SettingsRow.stepper("primehack-dark-cabin-gamma", L("Dark-Cabin Brightness (Gamma Lift)"), s.darkCabinGammaLift,
                          range: PrimeHackSettingsState.darkCabinGammaRange, step: 5,
                          format: { $0 <= 0.5 ? L("Off") : "+\(Int($0.rounded()))%" },
                          L("Lifts dark shadows (such as Dark Aether or Phazon Mines) so details stay readable at low cabin brightness."),
                          defaultValue: 0, set: { apply(.darkCabinGammaLift($0)) }),
      SettingsRow.destination("primehack-texture-status", L("HD Texture Pack Status"),
                              L("Check loaded texture packs on disk, live RAM cache usage against the safety cap, and eviction counts."),
                              icon: "photo.stack",
                              view: AnyView(PrimeHackTextureStatusView())),
      SettingsRow.toggle("primehack-crosshair", L("Crosshair (GameCube)"), s.gcCrosshair,
                         L("Draws a crosshair in Metroid Prime and Prime 2 on GameCube, which have none of their own."),
                         defaultValue: false, set: { apply(.gcCrosshair($0)) }),
      SettingsRow.picker("primehack-crosshair-color", L("Crosshair Colour (GameCube)"),
                         PrimeHackSettingsState.gcCrosshairColorOptions, s.gcCrosshairColor,
                         L("Colour of the crosshair in Metroid Prime and Prime 2 on GameCube."),
                         enabled: s.gcCrosshair,
                         defaultValue: PrimeHackSettingsState.defaultGCCrosshairColor,
                         disabledReason: L("Enable Crosshair (GameCube) first."),
                         set: { apply(.gcCrosshairColor($0)) }),
      SettingsRow.toggle("primehack-reduce-bloom", L("Reduce Bloom"), s.reduceBloom,
                         L("Tones down the glow spread in Metroid Prime 3."),
                         defaultValue: true, set: { apply(.reduceBloom($0)) }),
      SettingsRow.stepper("primehack-bloom-intensity", L("Bloom Intensity (Prime 3)"), s.bloomIntensity,
                          range: PrimeHackSettingsState.bloomIntensityRange, step: 5, format: percent,
                          L("Controls the strength of the bloom glow in Metroid Prime 3 (65% default, 100% original)."),
                          defaultValue: 65, set: { apply(.bloomIntensity($0)) }),
      SettingsRow.toggle("primehack-disable-bloom", L("Disable Bloom"), s.disableBloom,
                         L("Removes the glow effect entirely in Trilogy's Prime 1 and Prime 2, which can also make them a little faster. No effect on the GameCube games or Prime 3."),
                         defaultValue: false, set: { apply(.disableBloom($0)) }),
      SettingsRow.toggle("primehack-auto-efb", L("Automatic EFB Copies"), s.autoEFB,
                         L("Switches EFB copies to RAM only when the Scan Visor needs them, so scanning works without slowing the rest of the game."),
                         defaultValue: true, set: { apply(.autoEFB($0)) }),
      SettingsRow.toggle("primehack-arm", L("Reposition Arm Cannon"), s.armReposition,
                         L("Moves the arm cannon so it suits a wider field of view."),
                         defaultValue: true, set: { apply(.armReposition($0)) }),
      SettingsRow.picker("primehack-arm-mode", L("Arm Cannon Position"),
                         PrimeHackSettingsState.armPositionModeOptions, s.armPositionMode,
                         L("Auto scales the arm cannon with Field of View; Manual lets you dial in exact X/Y/Z offsets."),
                         enabled: s.armReposition,
                         defaultValue: 0,
                         disabledReason: L("Enable Reposition Arm Cannon first."),
                         set: { apply(.armPositionMode($0)) }),
    ]
    if s.armReposition && s.armPositionMode == 1 {
      visualItems.append(contentsOf: [
        SettingsRow.stepper("primehack-arm-x", L("Arm Left / Right"), s.armPositionLeftRight,
                            range: PrimeHackSettingsState.armOffsetRange, step: 5, format: whole,
                            L("Horizontal arm cannon offset (-50 left to +50 right)."),
                            defaultValue: 25, set: { apply(.armPositionLeftRight($0)) }),
        SettingsRow.stepper("primehack-arm-y", L("Arm Forward / Back"), s.armPositionForwardBack,
                            range: PrimeHackSettingsState.armOffsetRange, step: 5, format: whole,
                            L("Depth arm cannon offset (-50 back to +50 forward)."),
                            defaultValue: -35, set: { apply(.armPositionForwardBack($0)) }),
        SettingsRow.stepper("primehack-arm-z", L("Arm Up / Down"), s.armPositionUpDown,
                            range: PrimeHackSettingsState.armOffsetRange, step: 5, format: whole,
                            L("Vertical arm cannon offset (-50 down to +50 up)."),
                            defaultValue: -30, set: { apply(.armPositionUpDown($0)) }),
      ])
    }
    visualItems.append(contentsOf: [
      SettingsRow.toggle("primehack-gunfx", L("Secondary Gun Effects"), s.secondaryGunFX,
                         L("Restores the GameCube version's extra arm cannon beam effects in Trilogy's Metroid Prime."),
                         defaultValue: false, set: { apply(.secondaryGunFX($0)) }),
      SettingsRow.toggle("primehack-culling", L("Disable Culling"), s.toggleCulling,
                         L("Draws objects outside the original view so a wide field of view has no pop-in. Costs performance."),
                         defaultValue: false, set: { apply(.toggleCulling($0)) }),
    ])
    let visuals = MenuSection(id: "primehack-visuals", header: L("Visuals"), items: visualItems)

    let gameplay = MenuSection(id: "primehack-gameplay", header: L("Gameplay"),
                               footer: L("Changes apply straight away, in the games each one supports."), items: [
      SettingsRow.toggle("primehack-skip-cutscenes", L("Skippable Cutscenes"), s.skippableCutscenes,
                         L("Lets you skip cutscenes on a first playthrough, which the games normally only allow once you've finished them. Press Options (the game's Start or + button) during a cutscene."),
                         defaultValue: false, set: { apply(.skippableCutscenes($0)) }),
      SettingsRow.toggle("primehack-dashing", L("Restore Dashing"), s.restoreDashing,
                         L("Brings back the scan dash from the original GameCube releases."),
                         defaultValue: false, set: { apply(.restoreDashing($0)) }),
      SettingsRow.toggle("primehack-hud-memo", L("Disable HUD Pop-ups"), s.disableHudMemo,
                         L("Hides the pop-up messages that pause play when you pick up an item."),
                         defaultValue: false, set: { apply(.disableHudMemo($0)) }),
      SettingsRow.toggle("primehack-hypermode", L("Unlock Hypermode Difficulty"), s.unlockHypermode,
                         L("Makes Hypermode available from the start in Trilogy."),
                         defaultValue: true, set: { apply(.unlockHypermode($0)) }),
      SettingsRow.toggle("primehack-vouchers", L("Friend Vouchers"), s.friendVouchers,
                         L("Trilogy extras that normally also need Friend Vouchers can be bought with credits alone."),
                         defaultValue: false, set: { apply(.friendVouchers($0)) }),
      SettingsRow.toggle("primehack-mp2-portal", L("Skip Prime 2 Portal Cutscenes"), s.skipPortalMP2,
                         L("Skips the cutscene each time you go through a portal between Light and Dark Aether in Metroid Prime 2."),
                         defaultValue: false, set: { apply(.skipPortalMP2($0)) }),
      SettingsRow.toggle("primehack-any-beam", L("Doors Open with Any Beam"), s.anyBeamDoors,
                         L("Prime 1: a coloured door opens with any beam once you own the beam it needs. Prime 2: the Annihilator Beam opens every beam door."),
                         defaultValue: false, set: { apply(.anyBeamDoors($0)) }),
    ])

    let cheats = MenuSection(id: "primehack-cheats", header: L("Cheats"), items: [
      SettingsRow.toggle("primehack-invulnerable", L("Invulnerability"), s.invulnerability,
                         L("Samus takes no damage from most sources."),
                         defaultValue: false, set: { apply(.invulnerability($0)) }),
      SettingsRow.toggle("primehack-noclip", L("Noclip"), s.noclip,
                         L("Fly through walls. Can break your save if you leave a room the wrong way."),
                         defaultValue: false, set: { apply(.noclip($0)) }),
    ])

    #if os(iOS)
    let flight = MenuSection(id: "primehack-flight", header: L("On a Flight"),
                             footer: L("Applies in every game. The pause menu also shows the clock and iPad battery, includes a one-tap Flight/Quality preset, and saves your place automatically at 5% iPad battery (with Resume Where I Left Off on)."), items: [
      SettingsRow.destination("primehack-preflight", L("Pre-Flight Check"),
                              L("Verify signature expiry, Resume Where I Left Off, games, HD texture packs, free storage, and battery before boarding."),
                              icon: "airplane.departure",
                              view: AnyView(PrimeHackPreFlightCheckView())),
      SettingsRow.toggle("primehack-pause-headphones", L("Pause When Headphones Disconnect"), s.pauseOnHeadphonesOut,
                         L("Pauses the game when AirPods or other headphones disconnect or are unplugged, instead of carrying on through the iPad's speakers."),
                         defaultValue: true, set: { apply(.pauseOnHeadphonesOut($0)) }),
      SettingsRow.toggle("primehack-pause-after-sleep", L("Pause After Sleep & Reconnect"), s.pauseAfterSleep,
                         L("Lands on the Pause Menu when waking from sleep or switching back, and waits for a button press after a DualSense reconnects."),
                         defaultValue: true, set: { apply(.pauseAfterSleep($0)) }),
      SettingsRow.picker("primehack-idle-pause", L("Pause When Idle"),
                         PrimeHackSettingsState.idlePauseOptions, s.idlePauseMinutes,
                         L("After this long with no controller input, pauses the game and lets the iPad dim and lock as usual, so falling asleep mid-game does not drain the battery. Needs Controller Only. Any button press carries on."),
                         defaultValue: PrimeHackFlightOptions.defaultIdlePauseMinutes,
                         set: { apply(.idlePauseMinutes($0)) }),
      SettingsRow.toggle("primehack-thermal-res-stepdown", L("Thermal Resolution Step-Down"), s.thermalResStepDown,
                         L("If the iPad gets warm on a tray table, temporarily lowers internal resolution by 1 step (with a toast) instead of slowing the game, and restores it once cool."),
                         defaultValue: true, set: { apply(.thermalResStepDown($0)) }),
      SettingsRow.toggle("primehack-cabin-drc", L("Cabin-Noise Audio Mode (DRC)"), s.cabinDrcEnabled,
                         L("Gentle dynamic-range compression that lifts quiet ambience over cabin noise without making explosions harsh. Takes effect immediately."),
                         defaultValue: false, set: { apply(.cabinDrcEnabled($0)) }),
      SettingsRow.toggle("primehack-mix-audio", L("Play Alongside Music & Podcasts"), s.audioMixWithOthers,
                         L("Lets music or podcasts keep playing while you play PrimeHack instead of silencing other apps."),
                         defaultValue: false, set: { apply(.audioMixWithOthers($0)) }),
    ])
    #endif

    let performance = MenuSection(id: "primehack-performance", header: L("Performance"), items: [
      SettingsRow.toggle("primehack-gc-mp2-dual-core", L("Dual Core for Prime 2 (GameCube)"), s.gcPrime2DualCore,
                         L("Moves the graphics work of GameCube Metroid Prime 2 onto a second processor core, kept in step with the game, the way Trilogy's Prime 2 already runs. Can lift the frame rate in busy rooms. Experimental: if the game ever freezes, turn this off. Takes effect when a game starts."),
                         defaultValue: false, set: { apply(.gcPrime2DualCore($0)) }),
      SettingsRow.toggle("primehack-promotion-pacing", L("ProMotion 60 Hz Frame Pacing"), s.proMotionPacing,
                         L("Paces Metal presents to the game's refresh rate (16.7 ms on NTSC) on 120 Hz ProMotion screens for smoother motion. Experimental: turn off if a SyncGPU scene slows down."),
                         defaultValue: false, set: { apply(.proMotionPacing($0)) }),
    ])

    var creditItems: [MenuItem] = []
    if let signing = s.signingCaption {
      creditItems.append(SettingsRow.caption("primehack-signing", signing))
    }
    creditItems.append(SettingsRow.caption("primehack-credit", L("PrimeHack by shiiion, SirMangler and contributors. Running on iCube, DolphiniOS and Dolphin. Built-in JIT uses StikJIT, idevice and portions of Madeira. GPLv3+.")))
    let credits = MenuSection(id: "primehack-credits", items: creditItems)

    var sections: [MenuSection] = []
    if let jitSection {
      sections.append(jitSection)
    }
    #if os(iOS)
    sections.append(contentsOf: [controls, visuals, gameplay, flight, performance, cheats, credits])
    #else
    sections.append(contentsOf: [controls, visuals, gameplay, performance, cheats, credits])
    #endif
    return MenuModel(sections: sections)
  }
}

/// PrimeHack settings. Values live in Dolphin's Config, so they persist like any other setting.
struct PrimeHackSettingsView: View {
  @State private var state = PrimeHackSettingsState()
  #if os(iOS)
  @ObservedObject private var jit = BuiltInJitCoordinator.shared
  #endif

  var body: some View {
    SettingsLeafScreen(model: PrimeHackSettingsModelBuilder.make(state: state, jitSection: makeJitSection(), apply: apply),
                       title: L("PrimeHack"), sync: sync)
  }

  /// PrimeHack for iPad: built-in JIT section (status, Enable JIT, and JIT Setup).
  private func makeJitSection() -> MenuSection? {
    #if os(iOS) && !targetEnvironment(macCatalyst)
    guard JitManager.shared().jitSupported else { return nil }

    let statusBadge: String
    let statusDescription: String
    if jit.isJitActive {
      statusBadge = L("JIT active")
      statusDescription = jit.isGameRunning
        ? L("JIT is active and will apply from the next game start.")
        : L("JIT is active for this session. Games will boot with the JIT core.")
    } else if jit.isBusy {
      statusBadge = L("Working…")
      statusDescription = jit.statusMessage ?? L("Enabling JIT…")
    } else {
      statusBadge = L("Cached Interpreter")
      statusDescription = L("Games are currently using the Cached Interpreter.")
    }

    var items: [MenuItem] = [
      SettingsRow.action(
        "primehack-jit-status",
        L("Status"),
        statusDescription,
        enabled: false,
        badge: statusBadge,
        run: {}
      ),
      SettingsRow.action(
        "primehack-jit-enable",
        L("Enable JIT"),
        L("Enables JIT for this session using the built-in helper. First-time setup needs internet for the Developer Disk Image."),
        icon: "bolt.fill",
        enabled: !jit.isBusy && !jit.isJitActive,
        badge: jit.isJitActive ? L("Active") : (jit.isBusy ? L("Working…") : nil),
        disabledReason: jit.isJitActive
          ? L("JIT is already active for this session.")
          : L("Enabling JIT…"),
        run: { jit.enableJIT() }
      ),
    ]

    if jit.offerOpenLocalDevVPN && !jit.isJitActive {
      items.append(
        SettingsRow.action(
          "primehack-jit-open-vpn",
          L("Open LocalDevVPN"),
          L("Opens LocalDevVPN so you can connect the local tunnel, then returns to PrimeHack."),
          icon: "network",
          run: { jit.openLocalDevVPN() }
        )
      )
    }

    if let message = jit.errorMessage ?? jit.statusMessage {
      items.append(SettingsRow.caption("primehack-jit-message", message))
    }

    // PrimeHack for iPad: diagnostics without hunting through in-game menus.
    items.append(
      SettingsRow.action(
        "primehack-jit-diagnostics",
        L("Copy diagnostics"),
        L("Copies the emulator's state to the clipboard. Nothing is saved to a file."),
        icon: "doc.on.clipboard",
        run: { EmulationCoordinator.copyStateToClipboard() }
      )
    )

    items.append(
      SettingsRow.destination(
        "primehack-jit-setup",
        L("JIT Setup"),
        L("Import a pairing file, check LocalDevVPN, and prepare or reset the Developer Disk Image."),
        icon: "wrench.and.screwdriver",
        view: AnyView(BuiltInJitSetupView())
      )
    )

    let footer: String
    if jit.isJitActive && jit.isGameRunning {
      footer = L("JIT is active. Because a game is running, it will apply from the next game start.")
    } else if jit.isJitActive {
      footer = L("JIT is active. The next game you start will use the JIT core.")
    } else if !jit.pairingImported {
      footer = L("Import a pairing file in JIT Setup first, then connect LocalDevVPN and tap Enable JIT.")
    } else {
      footer = L("Speeds up games using the built-in JIT helper. Needs LocalDevVPN connected.")
    }

    return MenuSection(id: "primehack-jit", header: L("JIT"), footer: footer, items: items)
    #else
    return nil
    #endif
  }

  private func sync() {
    #if os(iOS)
    jit.refreshStatus()
    #endif
    var s = PrimeHackSettingsState()
    s.enabled = PrimeHackBridge.enabled()
    s.controllerOnly = ControllerManager.controllerOnly()
    s.adaptiveTriggers = PrimeHackAdaptiveTriggers.isEnabled()
    s.triggerStrength = PrimeHackAdaptiveTriggers.triggerStrengthLevel()
    s.dualSenseLightbar = PrimeHackAdaptiveTriggers.isLightbarEnabled()
    s.lightbarBrightness = PrimeHackAdaptiveTriggers.lightbarBrightnessLevel()
    s.damageMissileCues = PrimeHackAdaptiveTriggers.isDamageMissileCuesEnabled()
    s.rumbleStrength = Double(PrimeHackAdaptiveTriggers.rumbleStrengthPercent())
    s.rumbleMinPulseMs = PrimeHackAdaptiveTriggers.rumbleMinPulseMs()
    s.horizontalSensitivity = Double(PrimeHackBridge.stickHorizontalSensitivity())
    s.verticalSensitivity = Double(PrimeHackBridge.stickVerticalSensitivity())
    s.stickDeadzone = Double(PrimeHackBridge.stickDeadzone())
    s.stickOuterDeadzone = Double(PrimeHackBridge.stickOuterDeadzone())
    s.scanVisorSensitivity = Double(PrimeHackBridge.scanVisorSensitivity())
    s.turnAcceleration = Double(PrimeHackBridge.turnAcceleration())
    s.quick180Turn = PrimeHackBridge.quick180Turn()
    s.aimCurve = PrimeHackBridge.aimCurve()
    s.invertX = PrimeHackBridge.invertX()
    s.invertY = PrimeHackBridge.invertY()
    s.lockOnFreeAim = PrimeHackBridge.lockOnFreeAim()
    s.lockCameraInPuzzles = PrimeHackBridge.lockCameraInMotionPuzzles()
    s.perGameAimEnabled = PrimeHackBridge.perGameAimEnabled()
    s.fovEnabled = PrimeHackBridge.fovEnabled()
    s.fov = Double(PrimeHackBridge.fov())
    s.darkCabinGammaLift = Double(PrimeHackBridge.darkCabinGammaLift())
    s.gcCrosshair = PrimeHackBridge.showGameCubeCrosshair()
    s.gcCrosshairColor = PrimeHackBridge.gameCubeCrosshairColor()
    s.disableBloom = PrimeHackBridge.disableBloom()
    s.reduceBloom = PrimeHackBridge.reduceBloom()
    s.bloomIntensity = Double(PrimeHackBridge.bloomIntensity())
    s.autoEFB = PrimeHackBridge.autoEFB()
    s.armReposition = PrimeHackBridge.armReposition()
    s.armPositionMode = PrimeHackBridge.armPositionMode()
    s.armPositionLeftRight = Double(PrimeHackBridge.armPositionLeftRight())
    s.armPositionForwardBack = Double(PrimeHackBridge.armPositionForwardBack())
    s.armPositionUpDown = Double(PrimeHackBridge.armPositionUpDown())
    s.secondaryGunFX = PrimeHackBridge.secondaryGunFX()
    s.toggleCulling = PrimeHackBridge.toggleCulling()
    s.skippableCutscenes = PrimeHackBridge.skippableCutscenes()
    s.restoreDashing = PrimeHackBridge.restoreDashing()
    s.disableHudMemo = PrimeHackBridge.disableHudMemo()
    s.unlockHypermode = PrimeHackBridge.unlockHypermode()
    s.friendVouchers = PrimeHackBridge.friendVouchers()
    s.skipPortalMP2 = PrimeHackBridge.skipPortalMP2()
    s.anyBeamDoors = PrimeHackBridge.anyBeamDoors()
    s.invulnerability = PrimeHackBridge.invulnerability()
    s.noclip = PrimeHackBridge.noclip()
    s.gcPrime2DualCore = PrimeHackBridge.gcPrime2DualCore()
    s.proMotionPacing = PrimeHackBridge.proMotionPacing()
    s.pauseOnHeadphonesOut = PrimeHackFlightOptions.pauseOnHeadphonesOut()
    s.pauseAfterSleep = PrimeHackFlightOptions.pauseAfterSleep()
    s.idlePauseMinutes = PrimeHackFlightOptions.idlePauseMinutes()
    s.thermalResStepDown = PrimeHackFlightOptions.thermalResStepDown()
    s.cabinDrcEnabled = PrimeHackBridge.cabinDrcEnabled()
    s.audioMixWithOthers = PrimeHackBridge.audioMixWithOthers()
    s.signingCaption = AppSigningExpiry.caption()
    state = s
  }

  private func apply(_ change: PrimeHackSettingsChange) {
    switch change {
    case .enabled(let v): state.enabled = v; PrimeHackBridge.setEnabled(v)
    case .controllerOnly(let v): state.controllerOnly = v; ControllerManager.setControllerOnly(v)
    case .adaptiveTriggers(let v): state.adaptiveTriggers = v; PrimeHackAdaptiveTriggers.setEnabled(v)
    case .triggerStrength(let v): state.triggerStrength = v; PrimeHackAdaptiveTriggers.setTriggerStrengthLevel(v)
    case .dualSenseLightbar(let v): state.dualSenseLightbar = v; PrimeHackAdaptiveTriggers.setLightbarEnabled(v)
    case .lightbarBrightness(let v): state.lightbarBrightness = v; PrimeHackAdaptiveTriggers.setLightbarBrightnessLevel(v)
    case .damageMissileCues(let v): state.damageMissileCues = v; PrimeHackAdaptiveTriggers.setDamageMissileCuesEnabled(v)
    case .rumbleStrength(let v): state.rumbleStrength = v; PrimeHackAdaptiveTriggers.setRumbleStrengthPercent(Int(v.rounded()))
    case .rumbleMinPulseMs(let v): state.rumbleMinPulseMs = v; PrimeHackAdaptiveTriggers.setRumbleMinPulseMs(v)
    case .horizontalSensitivity(let v): state.horizontalSensitivity = v; PrimeHackBridge.setStickHorizontalSensitivity(Int(v.rounded()))
    case .verticalSensitivity(let v): state.verticalSensitivity = v; PrimeHackBridge.setStickVerticalSensitivity(Int(v.rounded()))
    case .stickDeadzone(let v): state.stickDeadzone = v; PrimeHackBridge.setStickDeadzone(Int(v.rounded()))
    case .stickOuterDeadzone(let v): state.stickOuterDeadzone = v; PrimeHackBridge.setStickOuterDeadzone(Int(v.rounded()))
    case .scanVisorSensitivity(let v): state.scanVisorSensitivity = v; PrimeHackBridge.setScanVisorSensitivity(Int(v.rounded()))
    case .turnAcceleration(let v): state.turnAcceleration = v; PrimeHackBridge.setTurnAcceleration(Int(v.rounded()))
    case .quick180Turn(let v): state.quick180Turn = v; PrimeHackBridge.setQuick180Turn(v)
    case .aimCurve(let v): state.aimCurve = v; PrimeHackBridge.setAimCurve(v)
    case .invertX(let v): state.invertX = v; PrimeHackBridge.setInvertX(v)
    case .invertY(let v): state.invertY = v; PrimeHackBridge.setInvertY(v)
    case .lockOnFreeAim(let v): state.lockOnFreeAim = v; PrimeHackBridge.setLockOnFreeAim(v)
    case .lockCameraInPuzzles(let v): state.lockCameraInPuzzles = v; PrimeHackBridge.setLockCameraInMotionPuzzles(v)
    case .fovEnabled(let v): state.fovEnabled = v; PrimeHackBridge.setFovEnabled(v)
    case .fov(let v): state.fov = v; PrimeHackBridge.setFov(Int(v.rounded()))
    case .darkCabinGammaLift(let v): state.darkCabinGammaLift = v; PrimeHackBridge.setDarkCabinGammaLift(Int(v.rounded()))
    case .gcCrosshair(let v): state.gcCrosshair = v; PrimeHackBridge.setShowGameCubeCrosshair(v)
    case .gcCrosshairColor(let v): state.gcCrosshairColor = v; PrimeHackBridge.setGameCubeCrosshairColor(v)
    case .disableBloom(let v): state.disableBloom = v; PrimeHackBridge.setDisableBloom(v)
    case .reduceBloom(let v): state.reduceBloom = v; PrimeHackBridge.setReduceBloom(v)
    case .bloomIntensity(let v): state.bloomIntensity = v; PrimeHackBridge.setBloomIntensity(Int(v.rounded()))
    case .autoEFB(let v): state.autoEFB = v; PrimeHackBridge.setAutoEFB(v)
    case .armReposition(let v): state.armReposition = v; PrimeHackBridge.setArmReposition(v)
    case .armPositionMode(let v): state.armPositionMode = v; PrimeHackBridge.setArmPositionMode(v)
    case .armPositionLeftRight(let v): state.armPositionLeftRight = v; PrimeHackBridge.setArmPositionLeftRight(Int(v.rounded()))
    case .armPositionForwardBack(let v): state.armPositionForwardBack = v; PrimeHackBridge.setArmPositionForwardBack(Int(v.rounded()))
    case .armPositionUpDown(let v): state.armPositionUpDown = v; PrimeHackBridge.setArmPositionUpDown(Int(v.rounded()))
    case .secondaryGunFX(let v): state.secondaryGunFX = v; PrimeHackBridge.setSecondaryGunFX(v)
    case .toggleCulling(let v): state.toggleCulling = v; PrimeHackBridge.setToggleCulling(v)
    case .skippableCutscenes(let v): state.skippableCutscenes = v; PrimeHackBridge.setSkippableCutscenes(v)
    case .restoreDashing(let v): state.restoreDashing = v; PrimeHackBridge.setRestoreDashing(v)
    case .disableHudMemo(let v): state.disableHudMemo = v; PrimeHackBridge.setDisableHudMemo(v)
    case .unlockHypermode(let v): state.unlockHypermode = v; PrimeHackBridge.setUnlockHypermode(v)
    case .friendVouchers(let v): state.friendVouchers = v; PrimeHackBridge.setFriendVouchers(v)
    case .skipPortalMP2(let v): state.skipPortalMP2 = v; PrimeHackBridge.setSkipPortalMP2(v)
    case .anyBeamDoors(let v): state.anyBeamDoors = v; PrimeHackBridge.setAnyBeamDoors(v)
    case .invulnerability(let v): state.invulnerability = v; PrimeHackBridge.setInvulnerability(v)
    case .noclip(let v): state.noclip = v; PrimeHackBridge.setNoclip(v)
    case .gcPrime2DualCore(let v): state.gcPrime2DualCore = v; PrimeHackBridge.setGcPrime2DualCore(v)
    case .proMotionPacing(let v): state.proMotionPacing = v; PrimeHackBridge.setProMotionPacing(v)
    case .pauseOnHeadphonesOut(let v): state.pauseOnHeadphonesOut = v; PrimeHackFlightOptions.setPauseOnHeadphonesOut(v)
    case .pauseAfterSleep(let v): state.pauseAfterSleep = v; PrimeHackFlightOptions.setPauseAfterSleep(v)
    case .idlePauseMinutes(let v): state.idlePauseMinutes = v; PrimeHackFlightOptions.setIdlePauseMinutes(v)
    case .thermalResStepDown(let v):
      state.thermalResStepDown = v
      PrimeHackFlightOptions.setThermalResStepDown(v)
      #if os(iOS)
      PrimeHackThermalStepDown.shared.evaluate()
      #endif
    case .cabinDrcEnabled(let v): state.cabinDrcEnabled = v; PrimeHackBridge.setCabinDrcEnabled(v)
    case .audioMixWithOthers(let v): state.audioMixWithOthers = v; PrimeHackBridge.setAudioMixWithOthers(v)
    }
  }
}

// MARK: - DualSense Controls Card (#7)

enum PrimeHackControlsCardBuilder {
  private static func row(_ id: String, _ action: String, _ binding: String, _ detail: String) -> MenuItem {
    // Keep enabled: true so DualSense D-pad / tvOS focus engine can navigate and scroll every row.
    SettingsRow.action(id, action, detail, enabled: true, badge: binding, run: {})
  }

  static func make(isWii: Bool? = nil) -> MenuModel {
    let core = MenuSection(id: "ph-card-core", header: L("Movement & Combat"), items: [
      row("ph-c-move", L("Move / Strafe"), "Left Stick", L("Move Samus or steer Morph Ball.")),
      row("ph-c-look", L("Camera Look"), "Right Stick", L("FPS camera look (no pointer needed).")),
      row("ph-c-fire", L("Fire / Charge Beam"), "R2", L("Tap to shoot; hold to charge.")),
      row("ph-c-lock", L("Lock-On / Scan"), "L2", L("Hold to lock onto targets or scan.")),
      row("ph-c-jump", L("Jump / Space Jump"), "✕ (Cross)", L("Jump or double-jump; Morph Ball bomb.")),
      row("ph-c-morph", L("Morph Ball"), "□ (Square)", L("Enter or exit Morph Ball.")),
      row("ph-c-missile", L("Missile / Power Bomb"), "△ (Triangle)", L("Fire Missile or drop Power Bomb in Morph Ball.")),
      row("ph-c-map", L("Map"), "R1", L("Open the area map (right stick rotates it).")),
      row("ph-c-grapple", L("Grapple / Lasso"), "○ (Circle)", L("Grapple Lasso in Prime 3 / Trilogy.")),
    ])

    let beams = MenuSection(id: "ph-card-beams", header: L("Beams & Visors"), items: [
      row("ph-c-b1", L("Beam 1 (Power)"), "D-Pad Up", L("Power Beam.")),
      row("ph-c-b2", L("Beam 2 (Wave / Dark)"), "D-Pad Right", L("Wave Beam (Prime 1) or Dark Beam (Prime 2).")),
      row("ph-c-b3", L("Beam 3 (Ice / Light)"), "D-Pad Down", L("Ice Beam (Prime 1) or Light Beam (Prime 2).")),
      row("ph-c-b4", L("Beam 4 (Plasma / Annihilator)"), "D-Pad Left", L("Plasma Beam (Prime 1) or Annihilator Beam (Prime 2).")),
      row("ph-c-v1", L("Combat Visor"), "L1 + D-Pad Up", L("Return to Combat Visor.")),
      row("ph-c-v2", L("Scan Visor"), "L1 + D-Pad Left", L("Switch to Scan Visor.")),
      row("ph-c-v3", L("Thermal / Dark / Command"), "L1 + D-Pad Down", L("Thermal (MP1), Dark (MP2), or Command Visor (MP3).")),
      row("ph-c-v4", L("X-Ray / Echo Visor"), "L1 + D-Pad Right", L("X-Ray (MP1/MP3) or Echo Visor (MP2).")),
    ])

    let extrasHeader = isWii == true
      ? L("Camera & Extras (Wii / Trilogy)")
      : isWii == false
        ? L("Camera & Extras (GameCube)")
        : L("Camera & Extras")
    let pitchBadge = isWii == true ? "L1 + R3" : (isWii == false ? "R3" : "R3 (GC) / L1 + R3 (Wii)")
    var extrasItems: [MenuItem] = [
      row("ph-c-180", L("Quick 180° Turn"), "L1 + L3", L("Smoothly spin 180° around in first-person.")),
      row("ph-c-pitch", L("Centre Camera Pitch"), pitchBadge, L("Re-centre vertical camera pitch to level.")),
      row("ph-c-spring", L("Spring Ball (Morph Ball)"), "L3", L("Jump while in Morph Ball (without holding L1).")),
      row("ph-c-pause", L("In-Game Pause / Hypermode"), "Options (+)", L("GameCube Start or Wii + (hold for Hypermode in Prime 3).")),
    ]
    if isWii != false {
      extrasItems.append(row("ph-c-minus", L("Wii Minus (−)"), "R3", L("Trilogy / Prime 3 minus button (without holding L1).")))
    }
    extrasItems.append(row("ph-c-menu", L("PrimeHack Pause Menu"), "Create / PS", L("Open PrimeHack's pause menu, saves, and Flight/Quality preset.")))
    let extras = MenuSection(id: "ph-card-extras", header: extrasHeader, items: extrasItems)

    return MenuModel(sections: [core, beams, extras])
  }
}

struct PrimeHackControlsCardView: View {
  var body: some View {
    SettingsLeafScreen(model: PrimeHackControlsCardBuilder.make(), title: L("DualSense Controls Card"), sync: {})
  }
}

// MARK: - Per-Game Aim Profiles (#12)

struct PrimeHackPerGameAimView: View {
  @State private var enabled = false
  @State private var mp1H = 45.0
  @State private var mp1V = 35.0
  @State private var mp1DZ = 10.0
  @State private var mp1FOV = 75.0
  @State private var mp2H = 45.0
  @State private var mp2V = 35.0
  @State private var mp2DZ = 10.0
  @State private var mp2FOV = 75.0
  @State private var mp3H = 45.0
  @State private var mp3V = 35.0
  @State private var mp3DZ = 10.0
  @State private var mp3FOV = 75.0

  var body: some View {
    SettingsLeafScreen(model: makeModel(), title: L("Per-Game Aim Profiles"), sync: sync)
  }

  private func sync() {
    enabled = PrimeHackBridge.perGameAimEnabled()
    mp1H = Double(PrimeHackBridge.mp1HSens())
    mp1V = Double(PrimeHackBridge.mp1VSens())
    mp1DZ = Double(PrimeHackBridge.mp1Deadzone())
    mp1FOV = Double(PrimeHackBridge.mp1Fov())
    mp2H = Double(PrimeHackBridge.mp2HSens())
    mp2V = Double(PrimeHackBridge.mp2VSens())
    mp2DZ = Double(PrimeHackBridge.mp2Deadzone())
    mp2FOV = Double(PrimeHackBridge.mp2Fov())
    mp3H = Double(PrimeHackBridge.mp3HSens())
    mp3V = Double(PrimeHackBridge.mp3VSens())
    mp3DZ = Double(PrimeHackBridge.mp3Deadzone())
    mp3FOV = Double(PrimeHackBridge.mp3Fov())
  }

  private func makeModel() -> MenuModel {
    let whole: (Double) -> String = { String(Int($0.rounded())) }
    let pct: (Double) -> String = { "\(Int($0.rounded()))%" }
    let deg: (Double) -> String = { "\(Int($0.rounded()))°" }

    let master = MenuSection(
      id: "ph-pga-master",
      header: L("Per-Game Profiles"),
      footer: L("When enabled, each game uses its own sensitivity, deadzone, and (when Custom Field of View is on) FOV below instead of the shared Controls sliders."),
      items: [
        SettingsRow.toggle("ph-pga-enable", L("Use Per-Game Aim Profiles"), enabled,
                           L("Override the shared look sensitivity, deadzone and FOV independently for Prime 1, Prime 2, and Prime 3."),
                           defaultValue: false, set: { v in enabled = v; PrimeHackBridge.setPerGameAimEnabled(v) }),
      ]
    )

    let s1 = MenuSection(id: "ph-pga-mp1", header: L("Metroid Prime 1 (GC & Trilogy)"), items: [
      SettingsRow.stepper("ph-mp1-h", L("Horizontal Sensitivity"), mp1H, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick horizontal speed in Prime 1."), enabled: enabled, defaultValue: 45,
                          set: { v in mp1H = v; PrimeHackBridge.setMp1HSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp1-v", L("Vertical Sensitivity"), mp1V, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick vertical speed in Prime 1."), enabled: enabled, defaultValue: 35,
                          set: { v in mp1V = v; PrimeHackBridge.setMp1VSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp1-dz", L("Deadzone"), mp1DZ, range: PrimeHackSettingsState.deadzoneRange, step: 2, format: pct,
                          L("Right-stick deadzone in Prime 1."), enabled: enabled, defaultValue: 10,
                          set: { v in mp1DZ = v; PrimeHackBridge.setMp1Deadzone(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp1-fov", L("Field of View"), mp1FOV, range: PrimeHackSettingsState.fovRange, step: 5, format: deg,
                          L("FOV in Prime 1 when Custom Field of View is enabled."), enabled: enabled, defaultValue: 75,
                          set: { v in mp1FOV = v; PrimeHackBridge.setMp1Fov(Int(v.rounded())) }),
    ])

    let s2 = MenuSection(id: "ph-pga-mp2", header: L("Metroid Prime 2: Echoes (GC & Trilogy)"), items: [
      SettingsRow.stepper("ph-mp2-h", L("Horizontal Sensitivity"), mp2H, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick horizontal speed in Prime 2."), enabled: enabled, defaultValue: 45,
                          set: { v in mp2H = v; PrimeHackBridge.setMp2HSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp2-v", L("Vertical Sensitivity"), mp2V, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick vertical speed in Prime 2."), enabled: enabled, defaultValue: 35,
                          set: { v in mp2V = v; PrimeHackBridge.setMp2VSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp2-dz", L("Deadzone"), mp2DZ, range: PrimeHackSettingsState.deadzoneRange, step: 2, format: pct,
                          L("Right-stick deadzone in Prime 2."), enabled: enabled, defaultValue: 10,
                          set: { v in mp2DZ = v; PrimeHackBridge.setMp2Deadzone(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp2-fov", L("Field of View"), mp2FOV, range: PrimeHackSettingsState.fovRange, step: 5, format: deg,
                          L("FOV in Prime 2 when Custom Field of View is enabled."), enabled: enabled, defaultValue: 75,
                          set: { v in mp2FOV = v; PrimeHackBridge.setMp2Fov(Int(v.rounded())) }),
    ])

    let s3 = MenuSection(id: "ph-pga-mp3", header: L("Metroid Prime 3: Corruption"), items: [
      SettingsRow.stepper("ph-mp3-h", L("Horizontal Sensitivity"), mp3H, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick horizontal speed in Prime 3."), enabled: enabled, defaultValue: 45,
                          set: { v in mp3H = v; PrimeHackBridge.setMp3HSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp3-v", L("Vertical Sensitivity"), mp3V, range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("Right-stick vertical speed in Prime 3."), enabled: enabled, defaultValue: 35,
                          set: { v in mp3V = v; PrimeHackBridge.setMp3VSens(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp3-dz", L("Deadzone"), mp3DZ, range: PrimeHackSettingsState.deadzoneRange, step: 2, format: pct,
                          L("Right-stick deadzone in Prime 3."), enabled: enabled, defaultValue: 10,
                          set: { v in mp3DZ = v; PrimeHackBridge.setMp3Deadzone(Int(v.rounded())) }),
      SettingsRow.stepper("ph-mp3-fov", L("Field of View"), mp3FOV, range: PrimeHackSettingsState.fovRange, step: 5, format: deg,
                          L("FOV in Prime 3 (above 94 costs extra GPU work)."), enabled: enabled, defaultValue: 75,
                          set: { v in mp3FOV = v; PrimeHackBridge.setMp3Fov(Int(v.rounded())) }),
    ])

    return MenuModel(sections: [master, s1, s2, s3])
  }
}

// MARK: - HD Texture Pack Scanner & Status Screen (#16)

struct PrimeHackTextureFolderSummary: Sendable, Equatable {
  let name: String
  let fileCount: Int
  let totalBytes: UInt64
}

enum PrimeHackTextureScanner {
  static func scanFolders(root: URL?) async -> [PrimeHackTextureFolderSummary] {
    guard let root else { return [] }
    return await Task.detached(priority: .utility) {
      Self.scanFoldersSync(root: root)
    }.value
  }

  // Synchronous helper so DirectoryEnumerator.makeIterator() is never invoked from an async context.
  private static func scanFoldersSync(root: URL) -> [PrimeHackTextureFolderSummary] {
    let fm = FileManager.default
    guard let entries = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey],
                                                    options: [.skipsHiddenFiles]) else { return [] }
    let exts: Set<String> = ["dds", "png", "jpg", "jpeg"]
    var results: [PrimeHackTextureFolderSummary] = []
    for folder in entries.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
      guard (try? folder.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true else { continue }
      var count = 0
      var bytes: UInt64 = 0
      if let enumerator = fm.enumerator(at: folder, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey],
                                        options: [.skipsHiddenFiles]) {
        for case let fileURL as URL in enumerator {
          guard exts.contains(fileURL.pathExtension.lowercased()) else { continue }
          if let vals = try? fileURL.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]),
             vals.isRegularFile == true {
            count += 1
            bytes += UInt64(max(0, vals.fileSize ?? 0))
          }
        }
      }
      if count > 0 {
        results.append(PrimeHackTextureFolderSummary(name: folder.lastPathComponent, fileCount: count, totalBytes: bytes))
      }
    }
    return results
  }

  static func formatBytes(_ bytes: UInt64) -> String {
    ByteCountFormatter.string(fromByteCount: Int64(clamping: bytes), countStyle: .file)
  }
}

struct PrimeHackTextureStatusView: View {
  @State private var hiresEnabled = false
  @State private var prefetchEnabled = false
  @State private var indexedCount: UInt = 0
  @State private var ramUsed: UInt64 = 0
  @State private var ramCap: UInt64 = 0
  @State private var activeAssets: UInt64 = 0
  @State private var pendingAssets: UInt64 = 0
  @State private var evictions: UInt64 = 0
  @State private var folders: [PrimeHackTextureFolderSummary] = []
  @State private var scanning = false

  var body: some View {
    SettingsLeafScreen(model: makeModel(), title: L("HD Texture Pack Status"), sync: sync)
  }

  private func sync() {
    hiresEnabled = DOLConfigBridge.gfxHiresTextures()
    prefetchEnabled = DOLConfigBridge.gfxCacheHiresTextures()
    indexedCount = PrimeHackBridge.hiresIndexedTextureCount()
    ramUsed = PrimeHackBridge.hiresCacheRamUsedBytes()
    ramCap = PrimeHackBridge.hiresCacheRamCapBytes()
    activeAssets = PrimeHackBridge.hiresCacheActiveCount()
    pendingAssets = PrimeHackBridge.hiresCachePendingCount()
    evictions = PrimeHackBridge.hiresCacheEvictionCount()
    guard !scanning else { return }
    scanning = true
    let root = ModContentInstaller.bridgedTexturesRoot()
    Task {
      let found = await PrimeHackTextureScanner.scanFolders(root: root)
      await MainActor.run {
        folders = found
        scanning = false
      }
    }
  }

  private func makeModel() -> MenuModel {
    let settingsSection = MenuSection(id: "ph-tex-settings", header: L("Settings"), items: [
      SettingsRow.action("ph-tex-enabled", L("Load Custom Textures"),
                         hiresEnabled ? L("Enabled — installed packs will be used when booting a game.")
                                      : L("Disabled — turn on Load Custom Textures under Graphics to use HD packs."),
                         enabled: true, badge: hiresEnabled ? L("On") : L("Off"), run: {}),
      SettingsRow.action("ph-tex-prefetch", L("Prefetch Custom Textures"),
                         prefetchEnabled ? L("Warning: Prefetch should stay OFF on iPad for multi-GB HD packs so iPadOS does not kill the app.")
                                         : L("Off (recommended for large HD packs)."),
                         enabled: true, badge: prefetchEnabled ? L("On (Risk)") : L("Off"), run: {}),
    ])

    let usedText = PrimeHackTextureScanner.formatBytes(ramUsed)
    let capText = ramCap > 0 ? PrimeHackTextureScanner.formatBytes(ramCap) : L("Not running")
    let pctText = ramCap > 0 ? " (\(min(100, Int((Double(ramUsed) / Double(ramCap) * 100.0).rounded())))%)" : ""
    let liveSection = MenuSection(id: "ph-tex-live", header: L("Live Session Cache"), items: [
      SettingsRow.action("ph-tex-indexed", L("Indexed for Active Game"),
                         L("Custom texture files matched for the currently running game."),
                         enabled: true, badge: "\(indexedCount)", run: {}),
      SettingsRow.action("ph-tex-ram", L("RAM Used / Safety Cap"),
                         L("Live HD texture memory usage compared to the iPadOS Jetsam safety cap."),
                         enabled: true, badge: "\(usedText) / \(capText)\(pctText)", run: {}),
      SettingsRow.action("ph-tex-active", L("Resident / Pending"),
                         L("Textures currently loaded in RAM vs queued on background loader threads."),
                         enabled: true, badge: "\(activeAssets) / \(pendingAssets)", run: {}),
      SettingsRow.action("ph-tex-evictions", L("LRU Evictions"),
                         L("Textures cleanly freed to stay below the memory cap."),
                         enabled: true, badge: "\(evictions)", run: {}),
    ])

    var diskItems: [MenuItem] = []
    if scanning && folders.isEmpty {
      diskItems.append(SettingsRow.caption("ph-tex-scanning", L("Scanning Load/Textures…")))
    } else if folders.isEmpty {
      diskItems.append(SettingsRow.caption("ph-tex-none", L("No HD texture folders found in Load/Textures.")))
    } else {
      for folder in folders {
        diskItems.append(
          SettingsRow.action("ph-tex-dir-\(folder.name)", folder.name,
                             String(format: L("%d texture files on disk."), folder.fileCount),
                             enabled: true,
                             badge: PrimeHackTextureScanner.formatBytes(folder.totalBytes),
                             run: {})
        )
      }
    }
    let diskSection = MenuSection(id: "ph-tex-disk", header: L("Installed Packs (Load/Textures)"), items: diskItems)
    return MenuModel(sections: [settingsSection, liveSection, diskSection])
  }
}

// MARK: - Pre-Flight Check Screen (#17)

struct PrimeHackPreFlightCheckView: View {
  @State private var signingBadge = ""
  @State private var signingDetail = ""
  @State private var resumeOn = false
  @State private var flightSummary = ""
  @State private var primeGamesCount = 0
  @State private var primeGamesList = ""
  @State private var textureBadge = ""
  @State private var textureDetail = ""
  @State private var storageBadge = ""
  @State private var controllerBadge = ""
  @State private var deviceBatteryBadge = ""
  @State private var scanning = false

  var body: some View {
    SettingsLeafScreen(model: makeModel(), title: L("Pre-Flight Check"), sync: sync)
  }

  private func sync() {
    if let signed = AppSigningExpiry.validity() {
      let days = AppSigningExpiry.daysRemaining(signed)
      let warn = AppSigningExpiry.warningDays(signed)
      let clamped = max(days, 0)
      signingBadge = clamped == 0 ? L("Expires today") : (clamped == 1 ? L("1 day left") : String(format: L("%d days left"), clamped))
      signingDetail = days <= warn
        ? L("Re-install PrimeHack via Sideloadly before you board so it cannot expire mid-trip.")
        : L("Signed and valid for offline travel.")
    } else {
      signingBadge = L("Unknown")
      signingDetail = L("No embedded provisioning profile found.")
    }

    resumeOn = SaveStateService.resumeEnabled
    let hp = PrimeHackFlightOptions.pauseOnHeadphonesOut()
    let slp = PrimeHackFlightOptions.pauseAfterSleep()
    let idle = PrimeHackFlightOptions.idlePauseMinutes()
    let therm = PrimeHackFlightOptions.thermalResStepDown()
    flightSummary = [
      hp ? L("Headphones") : nil,
      slp ? L("Sleep/Reconnect") : nil,
      idle > 0 ? String(format: L("Idle %dm"), idle) : nil,
      therm ? L("Thermal") : nil,
    ].compactMap { $0 }.joined(separator: " · ")

    let primePrefixes = ["GM8E", "GM8P", "G2ME", "G2MP", "R3ME", "R3MP", "RM3E", "RM3P"]
    let primeGames = TVLibraryBridge.currentGames().filter { item in
      primePrefixes.contains { item.gameID.hasPrefix($0) }
    }
    primeGamesCount = primeGames.count
    primeGamesList = primeGames.isEmpty
      ? L("No Metroid Prime discs found in your library.")
      : primeGames.map(\.title).joined(separator: ", ")

    let userURL = URL(fileURLWithPath: UserFolderUtil.getUserFolder(), isDirectory: true)
    if let freeBytes = DiscPatchService.availableCapacity(at: userURL) {
      storageBadge = PrimeHackTextureScanner.formatBytes(freeBytes)
    } else {
      storageBadge = L("Unknown")
    }

    if let p1 = ControllerBatterySnapshot.readPlayer1() {
      controllerBadge = p1.isCharging ? "\(p1.percent)% ⚡" : "\(p1.percent)%"
    } else if GCController.controllers().contains(where: { $0.extendedGamepad != nil }) {
      controllerBadge = L("Connected")
    } else {
      controllerBadge = L("Not connected")
    }

    #if os(iOS)
    let dev = DeviceStatusSnapshot.read()
    if let pct = dev.batteryPercent {
      deviceBatteryBadge = dev.isCharging ? "\(pct)% ⚡" : "\(pct)%"
    } else {
      deviceBatteryBadge = L("Unknown")
    }
    #else
    deviceBatteryBadge = L("Mains")
    #endif

    guard !scanning else { return }
    scanning = true
    let root = ModContentInstaller.bridgedTexturesRoot()
    let hiresOn = DOLConfigBridge.gfxHiresTextures()
    let prefetchOn = DOLConfigBridge.gfxCacheHiresTextures()
    Task {
      let packs = await PrimeHackTextureScanner.scanFolders(root: root)
      let totalFiles = packs.reduce(0) { $0 + $1.fileCount }
      let totalBytes = packs.reduce(UInt64(0)) { $0 + $1.totalBytes }
      await MainActor.run {
        scanning = false
        if packs.isEmpty {
          textureBadge = L("None")
          textureDetail = L("No HD texture packs found in Load/Textures.")
        } else {
          textureBadge = "\(packs.count) (\(PrimeHackTextureScanner.formatBytes(totalBytes)))"
          if !hiresOn {
            textureDetail = L("Packs found, but Load Custom Textures is OFF in Graphics.")
          } else if prefetchOn {
            textureDetail = L("Warning: turn Prefetch Custom Textures OFF before flying.")
          } else {
            textureDetail = String(format: L("%d textures ready (Prefetch OFF)."), totalFiles)
          }
        }
      }
    }
  }

  private func makeModel() -> MenuModel {
    let items: [MenuItem] = [
      SettingsRow.action("pf-signing", L("App Signature"), signingDetail, enabled: true, badge: signingBadge, run: {}),
      SettingsRow.action("pf-resume", L("Resume Where I Left Off"),
                         resumeOn ? L("Enabled — saves on pause, background, and 5% battery.")
                                  : L("Off — turn on in Settings so sleep or low battery never loses progress."),
                         enabled: true, badge: resumeOn ? L("On") : L("Off"), run: {}),
      SettingsRow.action("pf-protections", L("Flight Protections"),
                         flightSummary.isEmpty ? L("All flight protections are currently off.") : flightSummary,
                         enabled: true, badge: flightSummary.isEmpty ? L("Off") : L("Active"), run: {}),
      SettingsRow.action("pf-games", L("Metroid Prime Games"), primeGamesList,
                         enabled: true, badge: "\(primeGamesCount)", run: {}),
      SettingsRow.action("pf-textures", L("HD Texture Packs"),
                         scanning && textureBadge.isEmpty ? L("Scanning…") : textureDetail,
                         enabled: true, badge: textureBadge.isEmpty ? "…" : textureBadge, run: {}),
      SettingsRow.action("pf-storage", L("Free Storage"),
                         L("Available space for saves and state snapshots."),
                         enabled: true, badge: storageBadge, run: {}),
      SettingsRow.action("pf-controller", L("DualSense Controller"),
                         L("Pair and charge your DualSense before boarding."),
                         enabled: true, badge: controllerBadge, run: {}),
      SettingsRow.action("pf-device-battery", L("iPad Battery"),
                         L("Emergency resume snapshot triggers automatically at 5%."),
                         enabled: true, badge: deviceBatteryBadge, run: {}),
    ]
    return MenuModel(sections: [
      MenuSection(id: "pf-checklist", header: L("Ready for Takeoff"),
                  footer: L("Reopen this screen to refresh live battery, storage, and texture counts."),
                  items: items),
    ])
  }
}
