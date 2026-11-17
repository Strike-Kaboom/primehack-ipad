// Copyright 2026 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

import Foundation

/// The user's "Share crash reports" choice (Settings → Privacy). Read before Sentry starts, which is before
/// DefaultPreferences.plist is registered, so the default lives here, not in the plist.
/// PrimeHack for iPad: default OFF. Reports would go to the iCube team's Sentry project, and this is a
/// personal fork they did not build; the user can still opt in under Settings, Privacy.
enum TelemetryConsent {
  static let defaultsKey = "icube.telemetry.crashReports"

  static func isEnabled(_ defaults: UserDefaults = .standard) -> Bool {
    defaults.object(forKey: defaultsKey) as? Bool ?? false
  }

  static func set(_ on: Bool, defaults: UserDefaults = .standard) {
    defaults.set(on, forKey: defaultsKey)
  }
}
