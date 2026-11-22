// SPDX-License-Identifier: GPL-3.0-or-later
// Madeira Converter Exception: see LICENSE-EXCEPTION.md
// Portions derived from Madeira (https://github.com/willfaust/Madeira).
// Modified for PrimeHack for iPad, 2026-11-22.

import Foundation

/// PrimeHack for iPad: Codable messages shared by the app and its iCubeJITHelper extension.
/// Each travels as JSON inside an NSExtensionItem's userInfo dictionary.
struct BuiltInJitRequest: Codable, Sendable {
  static let itemKey = "primehack-jit-request"

  enum Operation: String, Codable, Sendable {
    case prepare
    case enable
    case resetDDI
  }

  let operation: Operation
  let targetPID: Int32?
  let pairingData: Data?

  static func prepare(pairingData: Data) -> BuiltInJitRequest {
    BuiltInJitRequest(operation: .prepare, targetPID: nil, pairingData: pairingData)
  }

  static func enable(targetPID: Int32, pairingData: Data) -> BuiltInJitRequest {
    BuiltInJitRequest(operation: .enable, targetPID: targetPID, pairingData: pairingData)
  }

  static var resetDDI: BuiltInJitRequest {
    BuiltInJitRequest(operation: .resetDDI, targetPID: nil, pairingData: nil)
  }

  struct Response: Codable, Sendable {
    static let itemKey = "primehack-jit-response"
    let success: Bool
    let message: String
    let txmPresent: Bool?
  }
}
