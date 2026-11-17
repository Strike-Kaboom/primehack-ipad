// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

import SwiftUI

/// Snapshot of PrimeHack's settings. Defaults match the core's Config defaults.
struct PrimeHackSettingsState: Equatable {
  var enabled = true
  var horizontalSensitivity = 45.0
  var verticalSensitivity = 35.0
  var invertX = false
  var invertY = false
  var lockCameraInPuzzles = true

  var fovEnabled = false
  var fov = 60.0
  var gcCrosshair = false
  var disableBloom = false
  var reduceBloom = true
  var autoEFB = true
  var armReposition = true
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

  static let sensitivityRange = 1.0 ... 100.0
  static let fovRange = 1.0 ... 170.0
}

/// One user edit. The view applies it to its snapshot AND to Config; the builder only emits it.
enum PrimeHackSettingsChange: Equatable {
  case enabled(Bool)
  case horizontalSensitivity(Double)
  case verticalSensitivity(Double)
  case invertX(Bool)
  case invertY(Bool)
  case lockCameraInPuzzles(Bool)
  case fovEnabled(Bool)
  case fov(Double)
  case gcCrosshair(Bool)
  case disableBloom(Bool)
  case reduceBloom(Bool)
  case autoEFB(Bool)
  case armReposition(Bool)
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
}

/// Pure: snapshot in, changes out.
enum PrimeHackSettingsModelBuilder {
  static func make(state s: PrimeHackSettingsState, apply: @escaping (PrimeHackSettingsChange) -> Void) -> MenuModel {
    let whole: (Double) -> String = { String(Int($0.rounded())) }
    let degrees: (Double) -> String = { "\(Int($0.rounded()))°" }

    let controls = MenuSection(id: "primehack-controls", header: L("Controls"),
                               footer: L("Applies to Metroid Prime, Prime 2 and Trilogy when a game controller is on player 1. Left stick moves, right stick looks. D-Pad picks a beam, L1 + D-Pad picks a visor. R2 fires, Cross jumps, Square morphs, Triangle fires a missile, L2 locks on, R1 opens the map."),
                               items: [
      SettingsRow.toggle("primehack-enabled", L("PrimeHack Controls"), s.enabled,
                         L("FPS-style camera control and quick beam and visor selection in the Metroid Prime games. Takes effect when a game starts."),
                         defaultValue: true, set: { apply(.enabled($0)) }),
      SettingsRow.stepper("primehack-h-sens", L("Look Sensitivity (Horizontal)"), s.horizontalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick turns the camera left and right."),
                          enabled: s.enabled, defaultValue: 45, set: { apply(.horizontalSensitivity($0)) }),
      SettingsRow.stepper("primehack-v-sens", L("Look Sensitivity (Vertical)"), s.verticalSensitivity,
                          range: PrimeHackSettingsState.sensitivityRange, step: 1, format: whole,
                          L("How fast the right stick tilts the camera up and down."),
                          enabled: s.enabled, defaultValue: 35, set: { apply(.verticalSensitivity($0)) }),
      SettingsRow.toggle("primehack-invert-x", L("Invert Horizontal Look"), s.invertX,
                         L("Pushing the right stick right turns the camera left."),
                         enabled: s.enabled, defaultValue: false, set: { apply(.invertX($0)) }),
      SettingsRow.toggle("primehack-invert-y", L("Invert Vertical Look"), s.invertY,
                         L("Pushing the right stick up looks down, like a flight stick."),
                         enabled: s.enabled, defaultValue: false, set: { apply(.invertY($0)) }),
      SettingsRow.toggle("primehack-lock-puzzles", L("Lock Camera in Motion Puzzles"), s.lockCameraInPuzzles,
                         L("Holds the camera still while you work a Trilogy motion puzzle, such as turning a lock."),
                         enabled: s.enabled, defaultValue: true, set: { apply(.lockCameraInPuzzles($0)) }),
    ])

    let visuals = MenuSection(id: "primehack-visuals", header: L("Visuals"), items: [
      SettingsRow.toggle("primehack-fov-enabled", L("Custom Field of View"), s.fovEnabled,
                         L("Replaces the game's field of view with the value below."),
                         defaultValue: false, set: { apply(.fovEnabled($0)) }),
      SettingsRow.stepper("primehack-fov", L("Field of View"), s.fov,
                          range: PrimeHackSettingsState.fovRange, step: 5, format: degrees,
                          L("Horizontal field of view in degrees. Around 75 to 90 suits a big screen."),
                          enabled: s.fovEnabled, defaultValue: 60, set: { apply(.fov($0)) }),
      SettingsRow.toggle("primehack-crosshair", L("Crosshair (GameCube)"), s.gcCrosshair,
                         L("Draws a crosshair in Metroid Prime and Prime 2 on GameCube, which have none of their own."),
                         defaultValue: false, set: { apply(.gcCrosshair($0)) }),
      SettingsRow.toggle("primehack-reduce-bloom", L("Reduce Bloom"), s.reduceBloom,
                         L("Tones down the glow in Metroid Prime 3."),
                         defaultValue: true, set: { apply(.reduceBloom($0)) }),
      SettingsRow.toggle("primehack-disable-bloom", L("Disable Bloom"), s.disableBloom,
                         L("Removes the glow effect entirely."),
                         defaultValue: false, set: { apply(.disableBloom($0)) }),
      SettingsRow.toggle("primehack-auto-efb", L("Automatic EFB Copies"), s.autoEFB,
                         L("Switches EFB copies to RAM only when the Scan Visor needs them, so scanning works without slowing the rest of the game."),
                         defaultValue: true, set: { apply(.autoEFB($0)) }),
      SettingsRow.toggle("primehack-arm", L("Reposition Arm Cannon"), s.armReposition,
                         L("Moves the arm cannon so it suits a wider field of view."),
                         defaultValue: true, set: { apply(.armReposition($0)) }),
      SettingsRow.toggle("primehack-gunfx", L("Secondary Gun Effects"), s.secondaryGunFX,
                         L("Keeps the extra beam effects on the arm cannon in Metroid Prime."),
                         defaultValue: false, set: { apply(.secondaryGunFX($0)) }),
      SettingsRow.toggle("primehack-culling", L("Disable Culling"), s.toggleCulling,
                         L("Draws objects outside the original view so a wide field of view has no pop-in. Costs performance."),
                         defaultValue: false, set: { apply(.toggleCulling($0)) }),
    ])

    let gameplay = MenuSection(id: "primehack-gameplay", header: L("Gameplay"),
                               footer: L("Takes effect when a game starts."), items: [
      SettingsRow.toggle("primehack-skip-cutscenes", L("Skippable Cutscenes"), s.skippableCutscenes,
                         L("Lets you skip cutscenes that normally can't be skipped."),
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
                         L("Gives you the Friend Vouchers used for some Trilogy extras."),
                         defaultValue: false, set: { apply(.friendVouchers($0)) }),
      SettingsRow.toggle("primehack-mp2-portal", L("Skip Prime 2 Portal Wait"), s.skipPortalMP2,
                         L("Skips the long wait at portals in Metroid Prime 2."),
                         defaultValue: false, set: { apply(.skipPortalMP2($0)) }),
      SettingsRow.toggle("primehack-any-beam", L("Doors Open with Any Beam"), s.anyBeamDoors,
                         L("Coloured doors open with whatever beam you fire."),
                         defaultValue: false, set: { apply(.anyBeamDoors($0)) }),
    ])

    let cheats = MenuSection(id: "primehack-cheats", header: L("Cheats"), items: [
      SettingsRow.toggle("primehack-invulnerable", L("Invulnerability"), s.invulnerability,
                         L("Samus takes no damage."),
                         defaultValue: false, set: { apply(.invulnerability($0)) }),
      SettingsRow.toggle("primehack-noclip", L("Noclip"), s.noclip,
                         L("Fly through walls. Can break your save if you leave a room the wrong way."),
                         defaultValue: false, set: { apply(.noclip($0)) }),
    ])

    let credits = MenuSection(id: "primehack-credits", items: [
      SettingsRow.caption("primehack-credit", L("PrimeHack by shiiion, SirMangler and contributors. Running on iCube, DolphiniOS and Dolphin. GPLv2+.")),
    ])

    return MenuModel(sections: [controls, visuals, gameplay, cheats, credits])
  }
}

/// PrimeHack settings. Values live in Dolphin's Config, so they persist like any other setting.
struct PrimeHackSettingsView: View {
  @State private var state = PrimeHackSettingsState()

  var body: some View {
    SettingsLeafScreen(model: PrimeHackSettingsModelBuilder.make(state: state, apply: apply), title: L("PrimeHack"), sync: sync)
  }

  private func sync() {
    var s = PrimeHackSettingsState()
    s.enabled = PrimeHackBridge.enabled()
    s.horizontalSensitivity = Double(PrimeHackBridge.stickHorizontalSensitivity())
    s.verticalSensitivity = Double(PrimeHackBridge.stickVerticalSensitivity())
    s.invertX = PrimeHackBridge.invertX()
    s.invertY = PrimeHackBridge.invertY()
    s.lockCameraInPuzzles = PrimeHackBridge.lockCameraInMotionPuzzles()
    s.fovEnabled = PrimeHackBridge.fovEnabled()
    s.fov = Double(PrimeHackBridge.fov())
    s.gcCrosshair = PrimeHackBridge.showGameCubeCrosshair()
    s.disableBloom = PrimeHackBridge.disableBloom()
    s.reduceBloom = PrimeHackBridge.reduceBloom()
    s.autoEFB = PrimeHackBridge.autoEFB()
    s.armReposition = PrimeHackBridge.armReposition()
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
    state = s
  }

  private func apply(_ change: PrimeHackSettingsChange) {
    switch change {
    case .enabled(let v): state.enabled = v; PrimeHackBridge.setEnabled(v)
    case .horizontalSensitivity(let v): state.horizontalSensitivity = v; PrimeHackBridge.setStickHorizontalSensitivity(Int(v.rounded()))
    case .verticalSensitivity(let v): state.verticalSensitivity = v; PrimeHackBridge.setStickVerticalSensitivity(Int(v.rounded()))
    case .invertX(let v): state.invertX = v; PrimeHackBridge.setInvertX(v)
    case .invertY(let v): state.invertY = v; PrimeHackBridge.setInvertY(v)
    case .lockCameraInPuzzles(let v): state.lockCameraInPuzzles = v; PrimeHackBridge.setLockCameraInMotionPuzzles(v)
    case .fovEnabled(let v): state.fovEnabled = v; PrimeHackBridge.setFovEnabled(v)
    case .fov(let v): state.fov = v; PrimeHackBridge.setFov(Int(v.rounded()))
    case .gcCrosshair(let v): state.gcCrosshair = v; PrimeHackBridge.setShowGameCubeCrosshair(v)
    case .disableBloom(let v): state.disableBloom = v; PrimeHackBridge.setDisableBloom(v)
    case .reduceBloom(let v): state.reduceBloom = v; PrimeHackBridge.setReduceBloom(v)
    case .autoEFB(let v): state.autoEFB = v; PrimeHackBridge.setAutoEFB(v)
    case .armReposition(let v): state.armReposition = v; PrimeHackBridge.setArmReposition(v)
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
    }
  }
}
