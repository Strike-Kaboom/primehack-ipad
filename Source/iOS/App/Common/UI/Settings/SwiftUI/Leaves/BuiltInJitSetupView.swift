// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
#if os(iOS)
import UniformTypeIdentifiers
#endif

/// PrimeHack for iPad: JIT Setup screen pushed from Settings › PrimeHack › JIT Setup.
/// Lets the user import a Remote Pairing (RPPairing) plist (from Files or Documents/JIT),
/// check/open LocalDevVPN, and prepare or reset the Developer Disk Image.
struct BuiltInJitSetupView: View {
  @ObservedObject private var jit = BuiltInJitCoordinator.shared
  #if os(iOS)
  @State private var showingFilePicker = false
  #endif

  var body: some View {
    SettingsLeafScreen(model: makeModel(), title: L("JIT Setup"), sync: sync)
      #if os(iOS)
      .sheet(isPresented: $showingFilePicker) {
        DocumentPickerView(contentTypes: [.propertyList, .data]) { urls in
          showingFilePicker = false
          if let url = urls.first {
            jit.importPairingFile(from: url)
          }
        }
      }
      #endif
  }

  private func sync() {
    jit.refreshStatus()
    jit.checkVPN()
  }

  private func makeModel() -> MenuModel {
    var sections: [MenuSection] = []

    if let feedback = jit.setupError ?? jit.setupMessage {
      sections.append(MenuSection(id: "jit-setup-feedback", header: L("Status"), items: [
        SettingsRow.caption("jit-setup-feedback-caption", feedback),
      ]))
    }

    // 1. Pairing File
    var pairingItems: [MenuItem] = [
      SettingsRow.action(
        "jit-setup-pairing-status",
        L("Pairing File"),
        jit.pairingImported
          ? L("A validated Remote Pairing file is stored securely in this device's Keychain.")
          : L("No pairing file imported yet. Export a Remote Pairing (RPPairing) plist with iloader or StikDebug."),
        enabled: false,
        badge: jit.pairingImported ? L("Imported") : L("Not imported"),
        run: {}
      ),
    ]
    #if os(iOS)
    pairingItems.append(
      SettingsRow.action(
        "jit-setup-import-file",
        L("Import Pairing File…"),
        L("Choose a Remote Pairing (RPPairing) plist from the Files app. It is validated and saved to the Keychain on this device only."),
        icon: "doc.badge.plus",
        enabled: !jit.isBusy,
        run: { showingFilePicker = true }
      )
    )
    #endif
    pairingItems.append(
      SettingsRow.action(
        "jit-setup-check-drop-folder",
        L("Check Documents/JIT Folder"),
        L("Looks for pairingFile.plist in Files › On My iPad › PrimeHack › JIT, imports it into the Keychain, and removes the copy from Documents."),
        icon: "folder",
        enabled: !jit.isBusy,
        run: { jit.importPairingFileFromDropFolder() }
      )
    )
    if jit.pairingImported {
      pairingItems.append(
        SettingsRow.destructive(
          "jit-setup-remove-pairing",
          L("Remove Pairing File"),
          L("Deletes the saved pairing file from this device's Keychain."),
          icon: "trash",
          enabled: !jit.isBusy,
          run: { jit.removePairingFile() }
        )
      )
    }
    sections.append(MenuSection(
      id: "jit-setup-pairing",
      header: L("Pairing File"),
      footer: L("You can also drop pairingFile.plist into Files › On My iPad › PrimeHack › JIT. PrimeHack moves it into the Keychain automatically."),
      items: pairingItems
    ))

    // 2. LocalDevVPN
    sections.append(MenuSection(
      id: "jit-setup-vpn",
      header: L("LocalDevVPN"),
      footer: L("LocalDevVPN lets PrimeHack's built-in helper reach this iPad locally. It works on Wi-Fi or offline, but not over mobile data alone."),
      items: [
        SettingsRow.action(
          "jit-setup-vpn-status",
          L("LocalDevVPN"),
          jit.vpnStatus.description,
          enabled: false,
          badge: jit.vpnStatus.badge,
          run: {}
        ),
        SettingsRow.action(
          "jit-setup-vpn-check",
          L("Check Connection"),
          L("Tests whether LocalDevVPN is currently routing to this iPad."),
          icon: "arrow.clockwise",
          enabled: !jit.isBusy,
          run: { jit.checkVPN() }
        ),
        SettingsRow.action(
          "jit-setup-vpn-open",
          L("Open LocalDevVPN"),
          L("Opens LocalDevVPN so you can connect the local tunnel, then returns to PrimeHack."),
          icon: "network",
          run: { jit.openLocalDevVPN() }
        ),
      ]
    ))

    // 3. Developer Disk Image
    sections.append(MenuSection(
      id: "jit-setup-ddi",
      header: L("Developer Disk Image"),
      footer: L("The first time you prepare the Developer Disk Image (or after an iPadOS update), this iPad needs an internet connection. Once cached and mounted, enabling JIT works offline."),
      items: [
        SettingsRow.action(
          "jit-setup-prepare-ddi",
          L("Prepare Developer Disk Image"),
          L("Downloads (if needed) and mounts the personalised Developer Disk Image for this iPadOS version. First time needs internet."),
          icon: "externaldrive.badge.checkmark",
          enabled: !jit.isBusy && jit.pairingImported,
          disabledReason: jit.isBusy
            ? L("A JIT operation is already in progress.")
            : L("Import a pairing file first."),
          run: { jit.prepareDeveloperDiskImage() }
        ),
        SettingsRow.destructive(
          "jit-setup-reset-ddi",
          L("Reset Developer Disk Image"),
          L("Clears the cached Developer Disk Image files stored by the built-in JIT helper. Use this if mounting fails after an iPadOS update."),
          icon: "trash",
          enabled: !jit.isBusy,
          run: { jit.resetDeveloperDiskImage() }
        ),
      ]
    ))

    return MenuModel(sections: sections)
  }
}
