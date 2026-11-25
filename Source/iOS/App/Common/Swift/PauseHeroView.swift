// Copyright 2026 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

import Combine
import GameController
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

/// Snapshot of Player 1's wireless controller battery (e.g. DualSense), shown in the Pause Menu
/// hero and monitored for low-battery toasts during gameplay.
struct ControllerBatterySnapshot: Equatable {
  let percent: Int
  let isCharging: Bool

  var symbolName: String {
    if isCharging { return "battery.100.bolt" }
    switch percent {
    case 88...: return "battery.100"
    case 63...: return "battery.75"
    case 38...: return "battery.50"
    case 13...: return "battery.25"
    default: return "battery.0"
    }
  }

  var tintColor: Color {
    if isCharging { return .green }
    if percent <= 10 { return .red }
    if percent <= 20 { return .orange }
    return ICubeDesign.color(.textPrimary)
  }

  @MainActor static func readPlayer1() -> ControllerBatterySnapshot? {
    let controllers = GCController.controllers()
    let preferred = controllers.first(where: { ControllerManager.shared.isPlayer1OfRunningSystem($0) })
      ?? controllers.first(where: { $0.battery != nil })
    guard let battery = preferred?.battery,
          battery.batteryState != .unknown,
          battery.batteryLevel >= 0 else { return nil }
    let pct = Int((min(max(battery.batteryLevel, 0), 1) * 100).rounded())
    let charging = (battery.batteryState == .charging || battery.batteryState == .full)
    return ControllerBatterySnapshot(percent: pct, isCharging: charging)
  }
}

#if os(iOS)
/// PrimeHack for iPad: the device's own clock and battery for the pause hero. A game hides the
/// status bar, so on a long flight the pause screen is the one place to check either.
struct DeviceStatusSnapshot: Equatable {
  let timeText: String
  /// nil when the system reports no level (Simulator, a Mac without a battery).
  let batteryPercent: Int?
  let isCharging: Bool

  var batterySymbolName: String {
    if isCharging { return "battery.100.bolt" }
    switch batteryPercent ?? 100 {
    case 88...: return "battery.100"
    case 63...: return "battery.75"
    case 38...: return "battery.50"
    case 13...: return "battery.25"
    default: return "battery.0"
    }
  }

  var batteryTint: Color {
    if isCharging { return .green }
    guard let percent = batteryPercent else { return ICubeDesign.color(.textPrimary) }
    if percent <= 10 { return .red }
    if percent <= 20 { return .orange }
    return ICubeDesign.color(.textPrimary)
  }

  @MainActor static func read(now: Date = Date()) -> DeviceStatusSnapshot {
    let device = UIDevice.current
    // Off by default; without it the level reads -1. Left on: the pause menu reopens often.
    if !device.isBatteryMonitoringEnabled { device.isBatteryMonitoringEnabled = true }
    let level = device.batteryLevel
    let state = device.batteryState
    let percent: Int? = (state == .unknown || level < 0) ? nil : Int((min(max(level, 0), 1) * 100).rounded())
    return DeviceStatusSnapshot(timeText: now.formatted(date: .omitted, time: .shortened),
                                batteryPercent: percent,
                                isCharging: state == .charging || state == .full)
  }
}

/// The clock and iPad battery chips under the pause hero's game ID row. Owns its own snapshot and
/// refresh timer, so the time keeps up while the pause menu stays open.
struct PauseDeviceStatusRow: View {
  @State private var status: DeviceStatusSnapshot?
  private let refreshTimer = Timer.publish(every: 15.0, on: .main, in: .common).autoconnect()

  var body: some View {
    HStack(spacing: ICubeDesign.Spacing.xs.rawValue) {
      if let status {
        chip(tint: ICubeDesign.color(.textPrimary)) {
          Image(systemName: "clock")
          Text(status.timeText)
        }
        if let percent = status.batteryPercent {
          chip(tint: status.batteryTint) {
            Image(systemName: "ipad.landscape")
            Image(systemName: status.batterySymbolName)
            Text("\(percent)%")
          }
        }
      }
    }
    .onAppear { status = DeviceStatusSnapshot.read() }
    .onReceive(refreshTimer) { now in status = DeviceStatusSnapshot.read(now: now) }
  }

  private func chip<Content: View>(tint: Color, @ViewBuilder content: () -> Content) -> some View {
    HStack(spacing: 3) { content() }
      .icubeText(.tag)
      .foregroundStyle(tint)
      .padding(.horizontal, ICubeDesign.Spacing.xs.rawValue)
      .padding(.vertical, ICubeDesign.Spacing.xxs.rawValue)
      .background(Capsule().fill(ICubeDesign.color(.rowSurface)))
  }
}
#endif

/// The pause screen's game hero (iFly layout, iCube tokens): the cover, "PAUSED" in the platform accent, the title in
/// the display face, the game ID and the running engine. tvOS and the iPad use the tall column beside the tiles; an
/// iPhone gets a banner above them (portrait) or a narrow column without the cover (landscape).
struct PauseHeroView: View {
  enum Layout {
    /// Cover above the text, `artWidth` wide.
    case column(artWidth: CGFloat)
    /// Cover beside the text, for the top of an iPhone screen.
    case banner
    /// No cover: an iPhone in landscape has no height for one.
    case textOnly
  }

  let game: TVGameItem
  let layout: Layout
  /// "JIT ON" while the JIT is acquired, otherwise the engine that is running (INT, CIR, CIR-IR).
  let engineChip: String
  let jitAcquired: Bool
  var controllerBattery: ControllerBatterySnapshot? = nil
  @Environment(\.icubePlatformAccent) private var platformAccent

  private typealias Metrics = ICubeDesign.PauseHero

  private var accent: Color { platformAccent ?? ICubeDesign.color(.accent) }

  var body: some View {
    switch layout {
    case .column(let artWidth):
      VStack(alignment: .leading, spacing: ICubeDesign.Spacing.m.rawValue) {
        art(width: artWidth)
          .shadow(color: .black.opacity(0.5), radius: ICubeDesign.TVPause.heroArtShadowRadius, x: 0, y: ICubeDesign.TVPause.heroArtShadowY)
          .padding(.bottom, ICubeDesign.Spacing.xs.rawValue)
        text(titleLines: Metrics.columnTitleLines)
      }
    case .banner:
      HStack(alignment: .center, spacing: ICubeDesign.Spacing.m.rawValue) {
        art(width: Metrics.bannerArtWidth)
          .shadow(color: .black.opacity(Metrics.bannerArtShadowOpacity), radius: Metrics.bannerArtShadowRadius, x: 0, y: Metrics.bannerArtShadowY)
        text(titleLines: Metrics.bannerTitleLines)
      }
    case .textOnly:
      text(titleLines: Metrics.columnTitleLines)
    }
  }

  private func text(titleLines: Int) -> some View {
    VStack(alignment: .leading, spacing: Metrics.textSpacing) {
      Label(L("Paused"), systemImage: "pause.fill")
        .font(Metrics.pausedFont)
        .icubeText(.nav)
        .tracking(Metrics.pausedTracking)
        .foregroundStyle(accent)
      titleText
        .lineLimit(titleLines)
        .minimumScaleFactor(Metrics.titleMinimumScale)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: ICubeDesign.Spacing.xs.rawValue) {
        Text(game.gameID)
          .icubeText(.tag)
          .foregroundStyle(ICubeDesign.color(.textSecondary))
        Text(engineChip)
          .icubeText(.tag)
          .foregroundStyle(ICubeDesign.color(.textPrimary))
          .padding(.horizontal, ICubeDesign.Spacing.xs.rawValue)
          .padding(.vertical, ICubeDesign.Spacing.xxs.rawValue)
          .background(Capsule().fill(jitAcquired ? ICubeDesign.color(.selectedFill) : ICubeDesign.color(.rowSurface)))
        if let battery = controllerBattery {
          HStack(spacing: 3) {
            Image(systemName: battery.symbolName)
            Text("\(battery.percent)%")
          }
          .icubeText(.tag)
          .foregroundStyle(battery.tintColor)
          .padding(.horizontal, ICubeDesign.Spacing.xs.rawValue)
          .padding(.vertical, ICubeDesign.Spacing.xxs.rawValue)
          .background(Capsule().fill(ICubeDesign.color(.rowSurface)))
        }
      }
      #if os(iOS)
      PauseDeviceStatusRow()
      #endif
    }
  }

  @ViewBuilder private var titleText: some View {
    #if os(tvOS)
    Text(game.title)
      .font(Metrics.titleFont)
      // Before `icubeText`: the role's own `textCase(nil)` would otherwise sit closer to the text and win.
      .textCase(.uppercase)
      .icubeText(.title)
      .foregroundStyle(LinearGradient(colors: [ICubeDesign.color(.textPrimary), accent], startPoint: .top, endPoint: .bottom))
    #else
    Text(game.title)
      .font(Metrics.titleFont)
      .icubeText(.title)
      .foregroundStyle(ICubeDesign.color(.textPrimary))
    #endif
  }

  /// The cover, filled into a 2:3 case and clipped; without one, the platform's case template on its accent, never a
  /// stretched placeholder glyph.
  private func art(width: CGFloat) -> some View {
    let shape = RoundedRectangle(cornerRadius: width < Metrics.smallArtThreshold ? ICubeDesign.Radius.small.rawValue : ICubeDesign.Radius.large.rawValue,
                                 style: .continuous)
    return ZStack {
      if game.hasCoverArt {
        Image(uiImage: game.coverImage).resizable().scaledToFill()
      } else {
        shape.fill(accent.opacity(ICubeDesign.IconBadge.tintOpacity))
        Image(LibraryGameSystem.from(item: game).templateImageName)
          .resizable().scaledToFill()
          .opacity(0.5)
      }
    }
    .frame(width: width, height: width * 3 / 2)
    .clipShape(shape)
    .overlay(shape.stroke(ICubeDesign.color(.hairline), lineWidth: ICubeDesign.Line.hairline.rawValue))
  }
}

extension ICubeDesign {
  enum PauseHero {
    static let columnTitleLines = 3
    static let bannerTitleLines = 2
    static let bannerArtWidth: CGFloat = 66
    static let bannerArtShadowOpacity: Double = 0.5
    static let bannerArtShadowRadius: CGFloat = 12
    static let bannerArtShadowY: CGFloat = 6
    /// Below this the cover takes the small radius.
    static let smallArtThreshold: CGFloat = 120
    static let titleMinimumScale: CGFloat = 0.6
    #if os(tvOS)
    static let textSpacing: CGFloat = Spacing.m.rawValue
    static let titleFont: Font = ICubeDesign.standard.font(.title)
    static let pausedFont: Font = ICubeDesign.standard.font(.nav)
    static let pausedTracking: CGFloat = TVPause.pausedTracking
    #else
    static let textSpacing: CGFloat = Spacing.xs.rawValue
    /// The title role is 34 pt on iOS, too big beside a cover at the top of a phone.
    static let titleFont: Font = ICubeDesign.displayFont(size: 26, relativeTo: .title2, bold: true)
    static let pausedFont: Font = ICubeDesign.displayFont(size: 12, relativeTo: .caption, bold: true)
    static let pausedTracking: CGFloat = 1.5
    #endif
  }
}
