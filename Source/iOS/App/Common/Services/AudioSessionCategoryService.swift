// Copyright 2022 DolphiniOS Project
// SPDX-License-Identifier: GPL-2.0-or-later

import UIKit

class AudioSessionCategoryService: UIResponder, UIApplicationDelegate {
  func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
    // Set the AVAudioSession category now.
    AudioSessionManager.shared().setSessionCategory()

    return true
  }

  func applicationDidBecomeActive(_ application: UIApplication) {
    // Reactivate AVAudioSession on wake/foreground before PauseArbiter resumes the core so
    // CoreAudio/AVAudioEngine don't fail with kAudioSessionNotActiveError after a long sleep.
    AudioSessionManager.shared().setSessionCategory()
  }
}
