// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation
import GameController
import UIKit

/// PrimeHack for iPad: DualSense adaptive triggers in the Metroid Prime games.
///
/// - R2 (fire): a gun-trigger "click" on each shot. Held, it builds a vibration like the charge beam
///   powering up, then holds a steady hum once a charge would be full (~1 s, the games' charge time).
/// - L2 (lock-on): light resistance.
///
/// Active only while a Prime game runs unpaused with PrimeHack controls and "Adaptive Triggers" on.
/// Everywhere else (menus, the pause menu, other games, the app in the background) the triggers are
/// switched off, so nothing leaks into other apps or games. Reads input by polling (30 Hz) rather
/// than installing handlers, so the app's own controller handlers are never replaced.
@MainActor
final class PrimeHackAdaptiveTriggers {
  static let shared = PrimeHackAdaptiveTriggers()

  static let enabledDefaultsKey = "primehack_adaptive_triggers"

  nonisolated static func isEnabled(in defaults: UserDefaults = .standard) -> Bool {
    defaults.register(defaults: [enabledDefaultsKey: true])
    return defaults.bool(forKey: enabledDefaultsKey)
  }

  nonisolated static func setEnabled(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: enabledDefaultsKey)
  }

  /// The trigger setup last written to a controller, so a mode is only sent when it changes.
  private enum RightMode: Equatable {
    case off
    case ready
    /// 1...10: how far the charge has built.
    case charging(Int)
    case charged
  }

  private struct PadState {
    var right: RightMode = .off
    var leftOn = false
    var pressStart: CFTimeInterval?
  }

  private var states: [ObjectIdentifier: PadState] = [:]
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
                 Notification.Name.GCControllerDidConnect] {
      center.addObserver(forName: name, object: nil, queue: .main) { _ in
        MainActor.assumeIsolated { PrimeHackAdaptiveTriggers.shared.refreshTimer() }
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
    refreshTimer()
  }

  /// Runs the 30 Hz poll only while a Prime game is loaded.
  private func refreshTimer() {
    guard Self.isPrimeGameRunning() else {
      switchAllOff()
      stopTimer()
      return
    }
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
    return ["GM8", "G2M", "R3M", "RM3"].contains { id.hasPrefix($0) }
  }

  private func tick() {
    guard Self.isPrimeGameRunning() else {
      switchAllOff()
      stopTimer()
      return
    }
    let active = Self.isEnabled() && PrimeHackBridge.enabled() && !TVEmulationBridge.isPaused()
      && UIApplication.shared.applicationState == .active
    let now = CACurrentMediaTime()
    var seen = Set<ObjectIdentifier>()
    for controller in GCController.controllers() {
      guard let pad = controller.extendedGamepad as? GCDualSenseGamepad else { continue }
      let id = ObjectIdentifier(controller)
      seen.insert(id)
      var state = states[id] ?? PadState()
      update(pad, state: &state, active: active, now: now)
      states[id] = state
    }
    states = states.filter { seen.contains($0.key) }
  }

  private func update(_ pad: GCDualSenseGamepad, state: inout PadState, active: Bool, now: CFTimeInterval) {
    guard active else {
      apply(.off, to: pad.rightTrigger, state: &state)
      setLeft(false, pad: pad, state: &state)
      state.pressStart = nil
      return
    }
    setLeft(true, pad: pad, state: &state)

    let right = pad.rightTrigger
    // Past the weapon snap point: the shot has fired and the button is held.
    if right.value > 0.6 {
      let start = state.pressStart ?? now
      state.pressStart = start
      let held = now - start
      if held < Self.chargeDelay {
        // Keep the weapon mode while the click happens.
        return
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

  private func apply(_ mode: RightMode, to trigger: GCDualSenseAdaptiveTrigger, state: inout PadState) {
    guard mode != state.right else { return }
    state.right = mode
    switch mode {
    case .off:
      trigger.setModeOff()
    case .ready:
      // A short, firm wall between 25% and 55% of the travel that gives way with a click.
      trigger.setModeWeaponWithStartPosition(0.25, endPosition: 0.55, resistiveStrength: 0.6)
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
    }
    states.removeAll()
  }
}
