// SPDX-License-Identifier: GPL-3.0-or-later
// Madeira Converter Exception: see LICENSE-EXCEPTION.md
// Portions derived from Madeira (https://github.com/willfaust/Madeira).
// Modified for PrimeHack for iPad, 2026-11-22.

import Foundation
import Security

/// PrimeHack for iPad: validates and stores this device's Remote Pairing (RPPairing) plist
/// in the Keychain (`kSecAttrAccessibleWhenUnlockedThisDeviceOnly`).
///
/// Users can import the pairing file either through the Files document picker in JIT Setup
/// or by placing `pairingFile.plist` in `Documents/JIT/` (visible in the Files app). Once
/// validated and saved to the Keychain, the copy in `Documents/JIT/` is removed while
/// `README.txt` stays behind. Pairing file contents are never logged.
enum BuiltInJitPairingStore {
  private static let service = "PrimeHackJITPairing"
  private static let account = "rppairing"

  private static let dropFolderName = "JIT"
  private static let dropFileName = "pairingFile.plist"
  private static let readmeFileName = "README.txt"

  private static let readmeContents = """
    PrimeHack for iPad — Built-in JIT Pairing File
    ==============================================

    To use the built-in Enable JIT button, place your device's Remote Pairing
    file (RPPairing plist, for example created with iloader or StikDebug) in
    this folder named:

      pairingFile.plist

    You can also import it from anywhere using Settings > PrimeHack > JIT Setup >
    Import Pairing File.

    Once PrimeHack imports the file, it validates it, stores it securely in this
    device's Keychain, and deletes the copy from this folder so your pairing keys
    are not left exposed in Files.

    You will also need LocalDevVPN connected when enabling JIT. The first time
    you prepare the Developer Disk Image (or after an iPadOS update), this iPad
    needs an internet connection; after that, enabling JIT works offline.
    """

  /// `Documents/JIT/` in the app container.
  static var dropFolderURL: URL? {
    FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?
      .appendingPathComponent(dropFolderName, isDirectory: true)
  }

  /// Creates `Documents/JIT/` and writes `README.txt` on first launch (idempotent).
  static func ensureDropFolderExists() {
    #if os(iOS) && !targetEnvironment(macCatalyst)
    guard let folder = dropFolderURL else { return }
    let manager = FileManager.default
    do {
      try manager.createDirectory(at: folder, withIntermediateDirectories: true)
      let readme = folder.appendingPathComponent(readmeFileName)
      if !manager.fileExists(atPath: readme.path) {
        try readmeContents.write(to: readme, atomically: true, encoding: .utf8)
      }
    } catch {
      NSLog("[PrimeHackJIT] Could not create Documents/JIT folder: %@", error.localizedDescription)
    }
    #endif
  }

  /// True when a validated pairing file is available (imports from `Documents/JIT/pairingFile.plist` first if present).
  static var isImported: Bool {
    (try? data()) != nil
  }

  /// Returns the stored RPPairing plist bytes from the Keychain, first importing
  /// `Documents/JIT/pairingFile.plist` if the user placed one there.
  static func data() throws -> Data {
    _ = try importFromDropFolderIfPresent()
    var result: AnyObject?
    let status = SecItemCopyMatching([
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: account,
      kSecReturnData as String: true,
      kSecMatchLimit as String: kSecMatchLimitOne,
    ] as CFDictionary, &result)
    guard status == errSecSuccess, let data = result as? Data, !data.isEmpty else {
      throw NSError(domain: "PrimeHackBuiltInJIT", code: 11,
                    userInfo: [NSLocalizedDescriptionKey:
                      "No pairing file is saved on this device yet. Open JIT Setup to import one, or place pairingFile.plist in Documents/JIT."])
    }
    return data
  }

  /// Checks `Documents/JIT/pairingFile.plist`. If present, validates and stores it in the
  /// Keychain, then removes the copy from Documents. Returns true if a file was imported.
  @discardableResult
  static func importFromDropFolderIfPresent() throws -> Bool {
    guard let folder = dropFolderURL else { return false }
    let file = folder.appendingPathComponent(dropFileName)
    let manager = FileManager.default
    guard manager.fileExists(atPath: file.path) else { return false }
    let bytes = try Data(contentsOf: file)
    try store(bytes)
    try? manager.removeItem(at: file)
    NSLog("[PrimeHackJIT] Imported pairing file from Documents/JIT into the Keychain")
    return true
  }

  /// Imports a pairing file picked from a document picker.
  static func importFile(from source: URL) throws {
    let scoped = source.startAccessingSecurityScopedResource()
    defer { if scoped { source.stopAccessingSecurityScopedResource() } }
    let bytes = try Data(contentsOf: source)
    try store(bytes)
    NSLog("[PrimeHackJIT] Imported pairing file into the Keychain")
  }

  /// Validates an RPPairing plist (32-byte public_key, 32-byte private_key, non-empty identifier)
  /// and saves it to the Keychain on this device only. Never logs its contents.
  static func store(_ data: Data) throws {
    guard !data.isEmpty,
          let plist = try? PropertyListSerialization.propertyList(from: data, options: [], format: nil),
          let dictionary = plist as? [String: Any],
          let publicKey = dictionary["public_key"] as? Data, publicKey.count == 32,
          let privateKey = dictionary["private_key"] as? Data, privateKey.count == 32,
          let identifier = dictionary["identifier"] as? String, !identifier.isEmpty else {
      throw NSError(domain: "PrimeHackBuiltInJIT", code: 10,
                    userInfo: [NSLocalizedDescriptionKey:
                      "This file isn't a valid Remote Pairing (RPPairing) plist. Export a pairing file with iloader or StikDebug and try again."])
    }
    let query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: account,
    ]
    SecItemDelete(query as CFDictionary)
    var add = query
    add[kSecValueData as String] = data
    add[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
    let status = SecItemAdd(add as CFDictionary, nil)
    guard status == errSecSuccess else {
      throw NSError(domain: "PrimeHackBuiltInJIT", code: 12,
                    userInfo: [NSLocalizedDescriptionKey:
                      "PrimeHack could not save the pairing file to the Keychain (error \(status)). Make sure this device is unlocked, then try again."])
    }
  }

  /// Removes any saved pairing file from the Keychain.
  static func remove() {
    let query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: account,
    ]
    SecItemDelete(query as CFDictionary)
    NSLog("[PrimeHackJIT] Removed saved pairing file from the Keychain")
  }
}
