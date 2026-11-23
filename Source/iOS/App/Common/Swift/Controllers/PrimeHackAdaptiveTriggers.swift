// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GameController
import UIKit

/// PrimeHack for iPad: DualSense adaptive triggers and visor/morph-ball lightbar sync in the
/// Metroid Prime games.
///
/// - R2 (fire): a gun-trigger "click" on each shot. Held, it builds a vibration like the charge beam
///   powering up, then holds a steady hum once a charge would be full (~1 s, the games' charge time).
///   Goes slack in Scan Visor and Morph Ball.
/// - L2 (lock-on / scan): light resistance during gameplay (slack in Morph Ball).
/// - Lightbar: softly tints to match the active visor or Morph Ball (dimmed for dark-cabin comfort)
///   and restores the original colour in menus, when paused, or when exiting.
///
/// Active only for player 1 during unpaused Prime gameplay with PrimeHack controls enabled for the
/// current run. Reads input by polling (30 Hz) rather than installing handlers, so the app's own
/// controller handlers are never replaced.
@MainActor
final class PrimeHackAdaptiveTriggers {
  static let shared = PrimeHackAdaptiveTriggers()

  static let enabledDefaultsKey = "primehack_adaptive_triggers"
  static let lightbarDefaultsKey = "primehack_dualsense_lightbar"
  private static let settingsChangedNotification = Notification.Name("PrimeHackDualSenseSettingsChanged")

  nonisolated static func isEnabled(in defaults: UserDefaults = .standard) -> Bool {
    // On by default. Read without register(defaults:), which the 30 Hz poll would repeat.
    (defaults.object(forKey: enabledDefaultsKey) as? Bool) ?? true
  }

  nonisolated static func setEnabled(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: enabledDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  nonisolated static func isLightbarEnabled(in defaults: UserDefaults = .standard) -> Bool {
    (defaults.object(forKey: lightbarDefaultsKey) as? Bool) ?? true
  }

  nonisolated static func setLightbarEnabled(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: lightbarDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  /// The trigger setup last written to a controller, so a mode is only sent when it changes.
  private enum RightMode: Equatable {
    case off
    case ready
    /// 1...10: how far the charge has built.
    case charging(Int)
    case charged
  }

  private struct LightTint: Equatable {
    let r: Float
    let g: Float
    let b: Float
  }

  private struct PadState {
    // A newly connected pad may already have a trigger effect applied.
    var right: RightMode?
    var leftOn: Bool?
    var pressStart: CFTimeInterval?
    /// True after entering active gameplay while R2 was already depressed (e.g. unpausing/unmorphing),
    /// suppressing a phantom click/charge until the player releases R2.
    var wasFireActive = false
    var waitForRightRelease = false
    var lightTint: LightTint?
    var originalLightColor: GCColor?
  }

  private var states: [ObjectIdentifier: PadState] = [:]
  private var cachedPlayer1ID: ObjectIdentifier?
  private var timer: Timer?
  private var started = false

  /// Seconds R2 must be held before the charge effect starts (a tap is just a shot).
  private static let chargeDelay: CFTimeInterval = 0.3
  /// Seconds after which a charge counts as full.
  private static let chargeFull: CFTimeInterval = 1.1

  private init() {}

  func start() {
    guard !started else { return }
    started = true
    let center = NotificationCenter.default
    for name in [Notification.Name("DOLEmulationDidStartNotification"),
                 UIApplication.didBecomeActiveNotification,
                 Notification.Name.GCControllerDidConnect,
                 Notification.Name.GCControllerDidDisconnect,
                 Self.settingsChangedNotification] {
      center.addObserver(forName: name, object: nil, queue: .main) { notification in
        let changedID = (notification.object as? GCController).map { ObjectIdentifier($0) }
        MainActor.assumeIsolated {
          let triggers = PrimeHackAdaptiveTriggers.shared
          // A disconnect and reconnect can occur between polls.
          if let changedID {
            triggers.states.removeValue(forKey: changedID)
          }
          triggers.refreshPlayer1Cache()
          triggers.refreshTimer()
        }
      }
    }
    center.addObserver(forName: ControllerManager.assignmentsChanged, object: nil, queue: .main) { _ in
      MainActor.assumeIsolated {
        PrimeHackAdaptiveTriggers.shared.refreshPlayer1Cache()
      }
      // Some callers post assignmentsChanged right before committing the new slot; refresh again
      // on the next main run-loop turn so the cache always reflects the final Player 1 device.
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          PrimeHackAdaptiveTriggers.shared.refreshPlayer1Cache()
        }
      }
    }
    for name in [Notification.Name("DOLEmulationDidEndNotification"),
                 UIApplication.willResignActiveNotification] {
      center.addObserver(forName: name, object: nil, queue: .main) { _ in
        MainActor.assumeIsolated {
          PrimeHackAdaptiveTriggers.shared.switchAllOff()
          PrimeHackAdaptiveTriggers.shared.stopTimer()
        }
      }
    }
    refreshPlayer1Cache()
    refreshTimer()
  }

  private func refreshPlayer1Cache() {
    guard Self.isPrimeGameRunning() else {
      cachedPlayer1ID = nil
      return
    }
    cachedPlayer1ID = nil
    for controller in GCController.controllers() {
      guard controller.extendedGamepad is GCDualSenseGamepad else { continue }
      if ControllerManager.shared.isPlayer1OfRunningSystem(controller) {
        cachedPlayer1ID = ObjectIdentifier(controller)
        break
      }
    }
  }

  private static func hasConnectedDualSense() -> Bool {
    GCController.controllers().contains { $0.extendedGamepad is GCDualSenseGamepad }
  }

  /// Runs the 30 Hz poll only while active, a Prime game is loaded, a DualSense is connected,
  /// and at least one DualSense feature (triggers or lightbar) is enabled.
  private func refreshTimer() {
    let anyFeatureEnabled = Self.isEnabled() || Self.isLightbarEnabled()
    guard UIApplication.shared.applicationState == .active,
          Self.isPrimeGameRunning(),
          anyFeatureEnabled,
          Self.hasConnectedDualSense() else {
      switchAllOff()
      stopTimer()
      return
    }
    refreshPlayer1Cache()
    guard timer == nil else { return }
    let t = Timer(timeInterval: 1.0 / 30.0, repeats: true) { _ in
      MainActor.assumeIsolated { PrimeHackAdaptiveTriggers.shared.tick() }
    }
    RunLoop.main.add(t, forMode: .common)
    timer = t
  }

  private func stopTimer() {
    timer?.invalidate()
    timer = nil
  }

  private static func isPrimeGameRunning() -> Bool {
    guard TVEmulationBridge.isRunning() else { return false }
    let id = TVEmulationBridge.currentGameID() as String
    // PrimeHack for iPad: match prime::kSupportedTitles (NTSC-U and PAL only) so unsupported
    // Japanese/other releases do not spin the 30 Hz trigger poll timer.
    return ["GM8E", "GM8P", "G2ME", "G2MP", "R3ME", "R3MP", "RM3E", "RM3P"].contains { id.hasPrefix($0) }
  }

  private static func lightTintForGameplay(morphBall: Bool, visorKind: Int) -> LightTint {
    // Softly dimmed (<= 0.45 channel brightness) so the lightbar is pleasant in a dark cabin.
    if morphBall {
      return LightTint(r: 0.45, g: 0.34, b: 0.03)  // Morph Ball gold
    }
    switch visorKind {
    case 1: return LightTint(r: 0.04, g: 0.34, b: 0.45)  // Scan Visor cyan
    case 2: return LightTint(r: 0.45, g: 0.08, b: 0.04)  // Thermal Visor crimson
    case 3: return LightTint(r: 0.14, g: 0.18, b: 0.45)  // X-Ray Visor icy violet-blue
    case 4: return LightTint(r: 0.28, g: 0.06, b: 0.42)  // Dark Visor deep purple
    case 5: return LightTint(r: 0.03, g: 0.42, b: 0.30)  // Echo Visor teal
    case 6: return LightTint(r: 0.06, g: 0.42, b: 0.14)  // Command Visor emerald
    default: return LightTint(r: 0.42, g: 0.22, b: 0.02) // Combat Visor / Varia amber
    }
  }

  private func tick() {
    let triggersEnabled = Self.isEnabled()
    let lightbarEnabled = Self.isLightbarEnabled()
    guard Self.isPrimeGameRunning(), (triggersEnabled || lightbarEnabled) else {
      switchAllOff()
      stopTimer()
      return
    }
    // Use the latched per-run PrimeHack flag so flipping Settings mid-game doesn't desync triggers.
    let inPlay = PrimeHackBridge.enabledForCurrentRun() && !TVEmulationBridge.isPaused()
      && UIApplication.shared.applicationState == .active && PrimeHackBridge.inGameplay()
    let morphBall = inPlay && PrimeHackBridge.inMorphBall()
    let scanVisor = inPlay && !morphBall && PrimeHackBridge.inScanVisor()
    let triggersActive = triggersEnabled && inPlay && !morphBall
    let lightTint: LightTint? = (lightbarEnabled && inPlay)
      ? Self.lightTintForGameplay(morphBall: morphBall, visorKind: PrimeHackBridge.activeVisorKind())
      : nil

    let now = CACurrentMediaTime()
    var seen = Set<ObjectIdentifier>()
    for controller in GCController.controllers() {
      guard let pad = controller.extendedGamepad as? GCDualSenseGamepad else { continue }
      let id = ObjectIdentifier(controller)
      seen.insert(id)
      var state = states[id] ?? PadState()
      let isPlayer1 = (cachedPlayer1ID == id)
      updateTriggers(pad, state: &state, active: isPlayer1 && triggersActive, scanVisor: scanVisor, now: now)
      updateLightbar(controller, state: &state, tint: isPlayer1 ? lightTint : nil)
      states[id] = state
    }
    states = states.filter { seen.contains($0.key) }
    if seen.isEmpty {
      stopTimer()
    }
  }

  private func updateTriggers(_ pad: GCDualSenseGamepad, state: inout PadState, active: Bool, scanVisor: Bool, now: CFTimeInterval) {
    guard active else {
      apply(.off, to: pad.rightTrigger, state: &state)
      setLeft(false, pad: pad, state: &state)
      state.pressStart = nil
      state.wasFireActive = false
      state.waitForRightRelease = false
      return
    }
    setLeft(true, pad: pad, state: &state)

    // In Scan Visor, R2 does not fire or charge: keep it slack while L2 still gives lock/scan feel.
    if scanVisor {
      apply(.off, to: pad.rightTrigger, state: &state)
      state.pressStart = nil
      state.wasFireActive = false
      state.waitForRightRelease = false
      return
    }

    let right = pad.rightTrigger
    if !state.wasFireActive {
      state.wasFireActive = true
      if right.value > 0.5 {
        // Trigger was already held across unpause/unmorph/visor exit: wait for release first.
        state.waitForRightRelease = true
      }
    }
    if state.waitForRightRelease {
      if right.value <= 0.35 {
        state.waitForRightRelease = false
      } else {
        apply(.off, to: right, state: &state)
        state.pressStart = nil
        return
      }
    }

    // Past the fire point (the emulator's button threshold is 0.5, just after the weapon snap at
    // 0.45): the shot has fired and the button is held.
    if right.value > 0.5 {
      let start = state.pressStart ?? now
      state.pressStart = start
      let held = now - start
      if held < Self.chargeDelay {
        // Keep the weapon mode while the click happens.
        apply(.ready, to: right, state: &state)
      } else if held < Self.chargeFull {
        let level = max(1, min(10, Int(((held - Self.chargeDelay) / (Self.chargeFull - Self.chargeDelay)) * 10) + 1))
        apply(.charging(level), to: right, state: &state)
      } else {
        apply(.charged, to: right, state: &state)
      }
    } else {
      state.pressStart = nil
      apply(.ready, to: right, state: &state)
    }
  }

  private func updateLightbar(_ controller: GCController, state: inout PadState, tint: LightTint?) {
    guard let light = controller.light else { return }
    if let tint {
      if state.originalLightColor == nil {
        state.originalLightColor = light.color
      }
      guard state.lightTint != tint else { return }
      state.lightTint = tint
      light.color = GCColor(red: tint.r, green: tint.g, blue: tint.b)
    } else if state.lightTint != nil {
      state.lightTint = nil
      if let original = state.originalLightColor {
        light.color = original
      }
    }
  }

  private func apply(_ mode: RightMode, to trigger: GCDualSenseAdaptiveTrigger, state: inout PadState) {
    guard mode != state.right else { return }
    state.right = mode
    switch mode {
    case .off:
      trigger.setModeOff()
    case .ready:
      // A short, firm wall between 25% and 45% of the travel that gives way with a click, so the
      // snap carries the trigger past the 0.5 fire point.
      trigger.setModeWeaponWithStartPosition(0.25, endPosition: 0.45, resistiveStrength: 0.6)
    case .charging(let level):
      let progress = Float(level) / 10
      trigger.setModeVibrationWithStartPosition(0.0, amplitude: 0.15 + 0.45 * progress,
                                                frequency: 0.12 + 0.18 * progress)
    case .charged:
      trigger.setModeVibrationWithStartPosition(0.0, amplitude: 0.7, frequency: 0.32)
    }
  }

  private func setLeft(_ on: Bool, pad: GCDualSenseGamepad, state: inout PadState) {
    guard on != state.leftOn else { return }
    state.leftOn = on
    if on {
      pad.leftTrigger.setModeFeedbackWithStartPosition(0.15, resistiveStrength: 0.25)
    } else {
      pad.leftTrigger.setModeOff()
    }
  }

  private func switchAllOff() {
    for controller in GCController.controllers() {
      guard let pad = controller.extendedGamepad as? GCDualSenseGamepad else { continue }
      pad.rightTrigger.setModeOff()
      pad.leftTrigger.setModeOff()
      let id = ObjectIdentifier(controller)
      if let original = states[id]?.originalLightColor, states[id]?.lightTint != nil {
        controller.light?.color = original
      }
    }
    states.removeAll()
  }
}
