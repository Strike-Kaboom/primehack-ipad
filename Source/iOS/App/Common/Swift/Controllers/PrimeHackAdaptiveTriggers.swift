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
  static let triggerStrengthDefaultsKey = "primehack_adaptive_trigger_strength"
  static let lightbarDefaultsKey = "primehack_dualsense_lightbar"
  static let lightbarBrightnessDefaultsKey = "primehack_lightbar_brightness"
  static let rumbleStrengthDefaultsKey = "primehack_rumble_strength"
  private static let settingsChangedNotification = Notification.Name("PrimeHackDualSenseSettingsChanged")

  nonisolated static func isEnabled(in defaults: UserDefaults = .standard) -> Bool {
    // On by default. Read without register(defaults:), which the 30 Hz poll would repeat.
    (defaults.object(forKey: enabledDefaultsKey) as? Bool) ?? true
  }

  nonisolated static func setEnabled(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: enabledDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  /// 0 = Light (0.65x), 1 = Medium (1.0x, default), 2 = Strong (1.25x).
  nonisolated static func triggerStrengthLevel(in defaults: UserDefaults = .standard) -> Int {
    guard let raw = defaults.object(forKey: triggerStrengthDefaultsKey) as? Int else { return 1 }
    return min(max(raw, 0), 2)
  }

  nonisolated static func setTriggerStrengthLevel(_ level: Int, in defaults: UserDefaults = .standard) {
    defaults.set(min(max(level, 0), 2), forKey: triggerStrengthDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  nonisolated static func isLightbarEnabled(in defaults: UserDefaults = .standard) -> Bool {
    (defaults.object(forKey: lightbarDefaultsKey) as? Bool) ?? true
  }

  nonisolated static func setLightbarEnabled(_ enabled: Bool, in defaults: UserDefaults = .standard) {
    defaults.set(enabled, forKey: lightbarDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  /// 0 = Subtle (0.45x), 1 = Medium (1.0x, default), 2 = Bright (1.8x).
  nonisolated static func lightbarBrightnessLevel(in defaults: UserDefaults = .standard) -> Int {
    guard let raw = defaults.object(forKey: lightbarBrightnessDefaultsKey) as? Int else { return 1 }
    return min(max(raw, 0), 2)
  }

  nonisolated static func setLightbarBrightnessLevel(_ level: Int, in defaults: UserDefaults = .standard) {
    defaults.set(min(max(level, 0), 2), forKey: lightbarBrightnessDefaultsKey)
    NotificationCenter.default.post(name: settingsChangedNotification, object: nil)
  }

  /// 25...100 percent, default 100.
  nonisolated static func rumbleStrengthPercent(in defaults: UserDefaults = .standard) -> Int {
    guard let raw = defaults.object(forKey: rumbleStrengthDefaultsKey) as? Int else { return 100 }
    return min(max(raw, 25), 100)
  }

  nonisolated static func setRumbleStrengthPercent(_ percent: Int, in defaults: UserDefaults = .standard) {
    defaults.set(min(max(percent, 25), 100), forKey: rumbleStrengthDefaultsKey)
  }

  private enum BeamFeel: Equatable {
    case power
    case heavy     // Ice / Dark
    case electric  // Wave / Annihilator
    case searing   // Plasma / Light / Phazon
    case hypermode // MP3 Hypermode
  }

  /// The trigger setup last written to a controller, so a mode is only sent when it changes.
  private enum RightMode: Equatable {
    case off
    case ready(BeamFeel, Int)
    /// 1...10: how far the charge has built.
    case charging(Int, BeamFeel, Int)
    case charged(BeamFeel, Int)
  }

  private struct LightTint: Equatable {
    let r: Float
    let g: Float
    let b: Float
  }

  private struct PadState {
    // A newly connected pad may already have a trigger effect applied.
    var right: RightMode?
    /// nil = unknown, -1 = off, 0..2 = on at triggerStrengthLevel.
    var leftState: Int?
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

  private static func clamp01(_ value: Float) -> Float {
    min(max(value, 0.0), 1.0)
  }

  private static func triggerStrengthScale(level: Int) -> Float {
    switch level {
    case 0: return 0.65
    case 2: return 1.25
    default: return 1.0
    }
  }

  private static func lightbarBrightnessScale(level: Int) -> Float {
    switch level {
    case 0: return 0.45
    case 2: return 1.8
    default: return 1.0
    }
  }

  private static func beamFeelFor(beamKind: Int, inHypermode: Bool) -> BeamFeel {
    if inHypermode { return .hypermode }
    switch beamKind {
    case 1, 5: return .heavy     // Ice / Dark
    case 2, 7: return .electric  // Wave / Annihilator
    case 3, 4, 6: return .searing // Plasma / Phazon / Light
    default: return .power
    }
  }

  private static func lightTintForGameplay(morphBall: Bool,
                                           visorKind: Int,
                                           beamKind: Int,
                                           inHypermode: Bool,
                                           lowHealth: Bool,
                                           brightnessLevel: Int,
                                           now: CFTimeInterval) -> LightTint {
    // Base palette softly dimmed (<= 0.48 channel) for dark-cabin comfort; scaled by brightnessLevel.
    var base: LightTint
    if morphBall {
      base = LightTint(r: 0.45, g: 0.34, b: 0.03)  // Morph Ball gold
    } else if inHypermode {
      base = LightTint(r: 0.04, g: 0.26, b: 0.50)  // MP3 Hypermode vivid Phazon blue
    } else {
      switch visorKind {
      case 1: base = LightTint(r: 0.04, g: 0.34, b: 0.45)  // Scan Visor cyan
      case 2: base = LightTint(r: 0.45, g: 0.08, b: 0.04)  // Thermal Visor crimson
      case 3: base = LightTint(r: 0.14, g: 0.18, b: 0.45)  // X-Ray Visor icy violet-blue
      case 4: base = LightTint(r: 0.28, g: 0.06, b: 0.42)  // Dark Visor deep purple
      case 5: base = LightTint(r: 0.03, g: 0.42, b: 0.30)  // Echo Visor teal
      case 6: base = LightTint(r: 0.06, g: 0.42, b: 0.14)  // Command Visor emerald
      default:
        // Combat Visor reflects the equipped beam.
        switch beamKind {
        case 1: base = LightTint(r: 0.06, g: 0.32, b: 0.45) // Ice Beam frost blue
        case 2: base = LightTint(r: 0.34, g: 0.08, b: 0.45) // Wave Beam electric violet
        case 3: base = LightTint(r: 0.45, g: 0.10, b: 0.03) // Plasma Beam fiery crimson-orange
        case 4: base = LightTint(r: 0.04, g: 0.22, b: 0.48) // Phazon Beam blue
        case 5: base = LightTint(r: 0.26, g: 0.06, b: 0.42) // Dark Beam indigo
        case 6: base = LightTint(r: 0.44, g: 0.40, b: 0.26) // Light Beam warm white-gold
        case 7: base = LightTint(r: 0.26, g: 0.34, b: 0.45) // Annihilator Beam silver-blue
        default: base = LightTint(r: 0.42, g: 0.22, b: 0.02) // Power Beam / Varia amber
        }
      }
    }

    if lowHealth {
      // Gentle 1 Hz breathing red pulse, quantized to 8 steps to keep Bluetooth HID traffic low.
      let rawWave = Float(0.5 + 0.5 * sin(now * 2.0 * .pi))
      let stepped = (rawWave * 8.0).rounded() / 8.0
      let blend: Float = 0.25 + 0.60 * stepped
      let alertR: Float = 0.46
      let alertG: Float = 0.03
      let alertB: Float = 0.02
      base = LightTint(
        r: base.r + (alertR - base.r) * blend,
        g: base.g + (alertG - base.g) * blend,
        b: base.b + (alertB - base.b) * blend
      )
    }

    let scale = lightbarBrightnessScale(level: brightnessLevel)
    return LightTint(r: clamp01(base.r * scale),
                     g: clamp01(base.g * scale),
                     b: clamp01(base.b * scale))
  }

  private func tick() {
    let triggersEnabled = Self.isEnabled()
    let lightbarEnabled = Self.isLightbarEnabled()
    guard Self.isPrimeGameRunning(), (triggersEnabled || lightbarEnabled) else {
      switchAllOff()
      stopTimer()
      return
    }
    let now = CACurrentMediaTime()
    // Use the latched per-run PrimeHack flag so flipping Settings mid-game doesn't desync triggers.
    let inPlay = PrimeHackBridge.enabledForCurrentRun() && !TVEmulationBridge.isPaused()
      && UIApplication.shared.applicationState == .active && PrimeHackBridge.inGameplay()
    let morphBall = inPlay && PrimeHackBridge.inMorphBall()
    let scanVisor = inPlay && !morphBall && PrimeHackBridge.inScanVisor()
    let hasChargeBeam = PrimeHackBridge.hasChargeBeam()
    let inHypermode = inPlay && PrimeHackBridge.inHypermode()
    let lowHealth = inPlay && PrimeHackBridge.isLowHealth()
    let visorKind = PrimeHackBridge.activeVisorKind()
    let beamKind = PrimeHackBridge.activeBeamKind()
    let triggerStrength = Self.triggerStrengthLevel()
    let brightnessLevel = Self.lightbarBrightnessLevel()
    let beamFeel = Self.beamFeelFor(beamKind: beamKind, inHypermode: inHypermode)

    let triggersActive = triggersEnabled && inPlay && !morphBall
    let lightTint: LightTint? = (lightbarEnabled && inPlay)
      ? Self.lightTintForGameplay(morphBall: morphBall,
                                  visorKind: visorKind,
                                  beamKind: beamKind,
                                  inHypermode: inHypermode,
                                  lowHealth: lowHealth,
                                  brightnessLevel: brightnessLevel,
                                  now: now)
      : nil

    var seen = Set<ObjectIdentifier>()
    for controller in GCController.controllers() {
      guard let pad = controller.extendedGamepad as? GCDualSenseGamepad else { continue }
      let id = ObjectIdentifier(controller)
      seen.insert(id)
      var state = states[id] ?? PadState()
      let isPlayer1 = (cachedPlayer1ID == id)
      updateTriggers(pad,
                     state: &state,
                     active: isPlayer1 && triggersActive,
                     scanVisor: scanVisor,
                     hasChargeBeam: hasChargeBeam,
                     beamFeel: beamFeel,
                     strengthLevel: triggerStrength,
                     now: now)
      updateLightbar(controller, state: &state, tint: isPlayer1 ? lightTint : nil)
      states[id] = state
    }
    states = states.filter { seen.contains($0.key) }
    if seen.isEmpty {
      stopTimer()
    }
  }

  private func updateTriggers(_ pad: GCDualSenseGamepad,
                              state: inout PadState,
                              active: Bool,
                              scanVisor: Bool,
                              hasChargeBeam: Bool,
                              beamFeel: BeamFeel,
                              strengthLevel: Int,
                              now: CFTimeInterval) {
    guard active else {
      apply(.off, to: pad.rightTrigger, state: &state)
      setLeft(false, strengthLevel: strengthLevel, pad: pad, state: &state)
      state.pressStart = nil
      state.wasFireActive = false
      state.waitForRightRelease = false
      return
    }
    setLeft(true, strengthLevel: strengthLevel, pad: pad, state: &state)

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
      if !hasChargeBeam {
        state.pressStart = nil
        apply(.ready(beamFeel, strengthLevel), to: right, state: &state)
        return
      }
      let start = state.pressStart ?? now
      state.pressStart = start
      let held = now - start
      if held < Self.chargeDelay {
        // Keep the weapon mode while the click happens.
        apply(.ready(beamFeel, strengthLevel), to: right, state: &state)
      } else if held < Self.chargeFull {
        let level = max(1, min(10, Int(((held - Self.chargeDelay) / (Self.chargeFull - Self.chargeDelay)) * 10) + 1))
        apply(.charging(level, beamFeel, strengthLevel), to: right, state: &state)
      } else {
        apply(.charged(beamFeel, strengthLevel), to: right, state: &state)
      }
    } else {
      state.pressStart = nil
      apply(.ready(beamFeel, strengthLevel), to: right, state: &state)
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
    case .ready(let feel, let strengthLevel):
      let scale = Self.triggerStrengthScale(level: strengthLevel)
      // A short, firm wall that gives way with a click just before the 0.5 fire point, tuned per beam.
      let (startPos, endPos, baseStrength): (Float, Float, Float)
      switch feel {
      case .power:     (startPos, endPos, baseStrength) = (0.25, 0.45, 0.60)
      case .heavy:     (startPos, endPos, baseStrength) = (0.20, 0.46, 0.78)
      case .electric:  (startPos, endPos, baseStrength) = (0.26, 0.44, 0.52)
      case .searing:   (startPos, endPos, baseStrength) = (0.24, 0.45, 0.72)
      case .hypermode: (startPos, endPos, baseStrength) = (0.22, 0.45, 0.85)
      }
      trigger.setModeWeaponWithStartPosition(startPos, endPosition: endPos,
                                             resistiveStrength: Self.clamp01(baseStrength * scale))
    case .charging(let level, let feel, let strengthLevel):
      let scale = Self.triggerStrengthScale(level: strengthLevel)
      let progress = Float(level) / 10
      let (ampMin, ampSpan, freqMin, freqSpan): (Float, Float, Float, Float)
      switch feel {
      case .power:     (ampMin, ampSpan, freqMin, freqSpan) = (0.15, 0.45, 0.12, 0.18)
      case .heavy:     (ampMin, ampSpan, freqMin, freqSpan) = (0.18, 0.50, 0.08, 0.14)
      case .electric:  (ampMin, ampSpan, freqMin, freqSpan) = (0.16, 0.46, 0.18, 0.22)
      case .searing:   (ampMin, ampSpan, freqMin, freqSpan) = (0.18, 0.48, 0.15, 0.21)
      case .hypermode: (ampMin, ampSpan, freqMin, freqSpan) = (0.22, 0.53, 0.16, 0.22)
      }
      trigger.setModeVibrationWithStartPosition(0.0,
                                                amplitude: Self.clamp01((ampMin + ampSpan * progress) * scale),
                                                frequency: Self.clamp01(freqMin + freqSpan * progress))
    case .charged(let feel, let strengthLevel):
      let scale = Self.triggerStrengthScale(level: strengthLevel)
      let (amp, freq): (Float, Float)
      switch feel {
      case .power:     (amp, freq) = (0.70, 0.32)
      case .heavy:     (amp, freq) = (0.76, 0.24)
      case .electric:  (amp, freq) = (0.72, 0.42)
      case .searing:   (amp, freq) = (0.76, 0.38)
      case .hypermode: (amp, freq) = (0.82, 0.40)
      }
      trigger.setModeVibrationWithStartPosition(0.0,
                                                amplitude: Self.clamp01(amp * scale),
                                                frequency: freq)
    }
  }

  private func setLeft(_ on: Bool, strengthLevel: Int, pad: GCDualSenseGamepad, state: inout PadState) {
    let desired = on ? strengthLevel : -1
    guard desired != state.leftState else { return }
    state.leftState = desired
    if on {
      let scale = Self.triggerStrengthScale(level: strengthLevel)
      pad.leftTrigger.setModeFeedbackWithStartPosition(0.15, resistiveStrength: Self.clamp01(0.25 * scale))
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
