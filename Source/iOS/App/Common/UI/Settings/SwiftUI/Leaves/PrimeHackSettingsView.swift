// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
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
}

/// Snapshot of PrimeHack's settings. Defaults match the core's Config defaults.
struct PrimeHackSettingsState: Equatable {
  var enabled = true
  var controllerOnly = true
  var adaptiveTriggers = true
  var triggerStrength = 1
  var dualSenseLightbar = true
  var lightbarBrightness = 1
  var rumbleStrength = 100.0
  var horizontalSensitivity = 45.0
  var verticalSensitivity = 35.0
  var stickDeadzone = 10.0
  var aimCurve = 0
  var invertX = false
  var invertY = false
  var lockOnFreeAim = false
  var lockCameraInPuzzles = true

  var fovEnabled = false
  var fov = 60.0
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

  var pauseOnHeadphonesOut = true
  var idlePauseMinutes = PrimeHackFlightOptions.defaultIdlePauseMinutes

  /// "Signed until …" for a sideloaded copy; nil when the build carries no provisioning profile.
  var signingCaption: String? = nil

  static let sensitivityRange = 1.0 ... 100.0
  static let deadzoneRange = 0.0 ... 30.0
  static let rumbleStrengthRange = 25.0 ... 100.0
  static let armOffsetRange = -50.0 ... 50.0
  static let fovRange = 1.0 ... 170.0
  static let bloomIntensityRange = 0.0 ... 100.0
  static let defaultGCCrosshairColor = 0x4b7ea331
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
  case rumbleStrength(Double)
  case horizontalSensitivity(Double)
  case verticalSensitivity(Double)
  case stickDeadzone(Double)
  case aimCurve(Int)
  case invertX(Bool)
  case invertY(Bool)
  case lockOnFreeAim(Bool)
  case lockCameraInPuzzles(Bool)
  case fovEnabled(Bool)
  case fov(Double)
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
  case pauseOnHeadphonesOut(Bool)
  case idlePauseMinutes(Int)
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
      SettingsRow.stepper("primehack-rumble-strength", L("Rumble Strength"), s.rumbleStrength,
                          range: PrimeHackSettingsState.rumbleStrengthRange, step: 5, format: percent,
                          L("Scales DualSense and device vibration intensity live without reconnecting the controller."),
                          defaultValue: 100, set: { apply(.rumbleStrength($0)) }),
      SettingsRow.stepper("primehack-h-sens", L("Look Sensitivity (Horizontal)"), s.horizontalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick turns the camera left and right."),
                          enabled: s.enabled, defaultValue: 45, set: { apply(.horizontalSensitivity($0)) }),
      SettingsRow.stepper("primehack-v-sens", L("Look Sensitivity (Vertical)"), s.verticalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick tilts the camera up and down."),
                          enabled: s.enabled, defaultValue: 35, set: { apply(.verticalSensitivity($0)) }),
      SettingsRow.stepper("primehack-deadzone", L("Right Stick Deadzone"), s.stickDeadzone,
                          range: PrimeHackSettingsState.deadzoneRange, step: 2, format: percent,
                          L("Ignores small stick deflections around centre to eliminate DualSense stick drift."),
                          enabled: s.enabled, defaultValue: 10, set: { apply(.stickDeadzone($0)) }),
      SettingsRow.picker("primehack-aim-curve", L("Aim Response Curve"),
                         PrimeHackSettingsState.aimCurveOptions, s.aimCurve,
                         L("Linear turns evenly with stick tilt; Smooth and Precision soften small tilts for finer aiming while keeping full turn speed at the edge."),
                         enabled: s.enabled, defaultValue: 0, set: { apply(.aimCurve($0)) }),
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
    ])

    var visualItems: [MenuItem] = [
      SettingsRow.toggle("primehack-fov-enabled", L("Custom Field of View"), s.fovEnabled,
                         L("Replaces the game's field of view with the value below."),
                         defaultValue: false, set: { apply(.fovEnabled($0)) }),
      SettingsRow.stepper("primehack-fov", L("Field of View"), s.fov,
                          range: PrimeHackSettingsState.fovRange, step: 5, format: degrees,
                          L("Horizontal field of view in degrees. Around 75 to 90 suits a big screen. Above 100 (Prime 3: above 94) the game also draws objects outside the view, which costs speed."),
                          enabled: s.fovEnabled, defaultValue: 60, set: { apply(.fov($0)) }),
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
                             footer: L("Both apply in every game. The pause menu also shows the time and the iPad's battery, and if the iPad's battery reaches 5% your place is saved once more (with Resume Where I Left Off on)."), items: [
      SettingsRow.toggle("primehack-pause-headphones", L("Pause When Headphones Disconnect"), s.pauseOnHeadphonesOut,
                         L("Pauses the game when AirPods or other headphones disconnect or are unplugged, instead of carrying on through the iPad's speakers."),
                         defaultValue: true, set: { apply(.pauseOnHeadphonesOut($0)) }),
      SettingsRow.picker("primehack-idle-pause", L("Pause When Idle"),
                         PrimeHackSettingsState.idlePauseOptions, s.idlePauseMinutes,
                         L("After this long with no controller input, pauses the game and lets the iPad dim and lock as usual, so falling asleep mid-game does not drain the battery. Needs Controller Only. Any button press carries on."),
                         defaultValue: PrimeHackFlightOptions.defaultIdlePauseMinutes,
                         set: { apply(.idlePauseMinutes($0)) }),
    ])
    #endif

    let performance = MenuSection(id: "primehack-performance", header: L("Performance"), items: [
      SettingsRow.toggle("primehack-gc-mp2-dual-core", L("Dual Core for Prime 2 (GameCube)"), s.gcPrime2DualCore,
                         L("Moves the graphics work of GameCube Metroid Prime 2 onto a second processor core, kept in step with the game, the way Trilogy's Prime 2 already runs. Can lift the frame rate in busy rooms. Experimental: if the game ever freezes, turn this off. Takes effect when a game starts."),
                         defaultValue: false, set: { apply(.gcPrime2DualCore($0)) }),
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
    s.rumbleStrength = Double(PrimeHackAdaptiveTriggers.rumbleStrengthPercent())
    s.horizontalSensitivity = Double(PrimeHackBridge.stickHorizontalSensitivity())
    s.verticalSensitivity = Double(PrimeHackBridge.stickVerticalSensitivity())
    s.stickDeadzone = Double(PrimeHackBridge.stickDeadzone())
    s.aimCurve = PrimeHackBridge.aimCurve()
    s.invertX = PrimeHackBridge.invertX()
    s.invertY = PrimeHackBridge.invertY()
    s.lockOnFreeAim = PrimeHackBridge.lockOnFreeAim()
    s.lockCameraInPuzzles = PrimeHackBridge.lockCameraInMotionPuzzles()
    s.fovEnabled = PrimeHackBridge.fovEnabled()
    s.fov = Double(PrimeHackBridge.fov())
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
    s.pauseOnHeadphonesOut = PrimeHackFlightOptions.pauseOnHeadphonesOut()
    s.idlePauseMinutes = PrimeHackFlightOptions.idlePauseMinutes()
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
    case .rumbleStrength(let v): state.rumbleStrength = v; PrimeHackAdaptiveTriggers.setRumbleStrengthPercent(Int(v.rounded()))
    case .horizontalSensitivity(let v): state.horizontalSensitivity = v; PrimeHackBridge.setStickHorizontalSensitivity(Int(v.rounded()))
    case .verticalSensitivity(let v): state.verticalSensitivity = v; PrimeHackBridge.setStickVerticalSensitivity(Int(v.rounded()))
    case .stickDeadzone(let v): state.stickDeadzone = v; PrimeHackBridge.setStickDeadzone(Int(v.rounded()))
    case .aimCurve(let v): state.aimCurve = v; PrimeHackBridge.setAimCurve(v)
    case .invertX(let v): state.invertX = v; PrimeHackBridge.setInvertX(v)
    case .invertY(let v): state.invertY = v; PrimeHackBridge.setInvertY(v)
    case .lockOnFreeAim(let v): state.lockOnFreeAim = v; PrimeHackBridge.setLockOnFreeAim(v)
    case .lockCameraInPuzzles(let v): state.lockCameraInPuzzles = v; PrimeHackBridge.setLockCameraInMotionPuzzles(v)
    case .fovEnabled(let v): state.fovEnabled = v; PrimeHackBridge.setFovEnabled(v)
    case .fov(let v): state.fov = v; PrimeHackBridge.setFov(Int(v.rounded()))
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
    case .pauseOnHeadphonesOut(let v): state.pauseOnHeadphonesOut = v; PrimeHackFlightOptions.setPauseOnHeadphonesOut(v)
    case .idlePauseMinutes(let v): state.idlePauseMinutes = v; PrimeHackFlightOptions.setIdlePauseMinutes(v)
    }
  }
}
