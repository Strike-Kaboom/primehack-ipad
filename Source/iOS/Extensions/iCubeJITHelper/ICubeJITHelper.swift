// SPDX-License-Identifier: GPL-3.0-or-later
// Licence notices: see Source/iOS/App/Common/Resources/Licenses/THIRD-PARTY-JIT.md
// Portions copyright 2026 alantangok: code taken from https://github.com/willfaust/Madeira (pull request #541)
// under GPL-3.0-or-later and modified for BCD in November 2026.
// Modified for PrimeHack for iPad, 2026-11-22.

// PrimeHack for iPad: built-in JIT helper extension (classic NSExtension, loaded dynamically
// by bundle ID from PlugIns/iCubeJITHelper.appex so sideloaders that rename the bundle ID
// still resolve it). Runs StikJIT with its built-in `.universal` script and `forceScript: true`.

import Foundation
#if !targetEnvironment(simulator)
import StikJIT
#endif

#if targetEnvironment(simulator)
private struct DDIPaths {
  static func `default`(in directory: URL) -> DDIPaths { DDIPaths() }
}

private enum StikJIT {
  enum Script {
    case universal
    case legacy
    case custom(URL)
    case customBase64(String)
  }
  struct DeviceSecurityState { let isTXMPresent: Bool? }
  enum PreparationStage { case unavailable }
  enum JITReadiness {
    case unreachable(reason: String)
    case preparationFailed(reason: String)
    case ready(DeviceSecurityState)
  }

  static var isTXMPresent: Bool? { nil }

  static func prepareDevice(pairingFile: URL, paths: DDIPaths,
                            progress: (PreparationStage) -> Void) -> JITReadiness {
    .unreachable(reason: "Built-in JIT works only on a real device, not in the simulator.")
  }

  static func enableJIT(targetPID: Int32, pairingFile: URL, ddiPaths: DDIPaths,
                        script: Script, forceScript: Bool,
                        preparationProgress: (PreparationStage) -> Void,
                        progress: (String) -> Void) throws {
    throw NSError(domain: "ICubeJITHelper", code: 100,
                  userInfo: [NSLocalizedDescriptionKey:
                    "Built-in JIT works only on a real device, not in the simulator."])
  }

  static func resetCachedDDI(at paths: DDIPaths) throws {}
  static func isDDIMounted(pairingFile: URL) throws -> Bool { false }
}
#endif

private enum ICubeJITWork {
  /// Serialise helper work one request at a time so DDI operations cannot race.
  static let queue = DispatchQueue(label: "com.strikekaboom.primehack.jit-helper")

  /// Another preparation may install the cryptex after StikJIT's initial check
  /// ("system: cryptex already exists", com.apple.security.cryptex 17).
  /// Verify that it is now mounted, then retry the prepare operation once.
  static func retryInstalledCryptex<T>(verifyMounted: () throws -> Bool,
                                       operation: () throws -> T) throws -> T {
    do { return try operation() }
    catch {
      let message = error.localizedDescription.lowercased()
      guard message.contains("cryptexd("), message.contains("routine: \"install\""),
            message.contains("system: cryptex already exists:"),
            message.contains("com.apple.security.cryptex: 17)"),
            (try? verifyMounted()) == true else { throw error }
      NSLog("[PrimeHackJIT] DDI cryptex already installed and mounted; retrying once")
      return try operation()
    }
  }

  static func handle(_ request: BuiltInJitRequest) -> BuiltInJitRequest.Response {
    let manager = FileManager.default
    let root = manager.urls(for: .libraryDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("StikJIT", isDirectory: true)
    let paths = DDIPaths.default(in: root)

    do {
      try manager.createDirectory(at: root, withIntermediateDirectories: true)
      // Sweep any temporary pairing files left behind if a previous invocation was SIGKILLed.
      if let leftover = try? manager.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) {
        for url in leftover where url.lastPathComponent.hasPrefix("pairingFile-") && url.pathExtension == "plist" {
          try? manager.removeItem(at: url)
        }
      }
      if request.operation == .resetDDI {
        try StikJIT.resetCachedDDI(at: paths)
        return .init(success: true,
                     message: "The cached Developer Disk Image was cleared.",
                     txmPresent: StikJIT.isTXMPresent)
      }

      guard let pairingData = request.pairingData, !pairingData.isEmpty else {
        throw NSError(domain: "ICubeJITHelper", code: 1,
                      userInfo: [NSLocalizedDescriptionKey: "No pairing file was included with the request."])
      }
      // Write pairing data to a temporary file inside the extension's Library/StikJIT directory
      // and delete it immediately when done; never log its contents.
      let pairingURL = root.appendingPathComponent("pairingFile-\(UUID().uuidString).plist")
      try pairingData.write(to: pairingURL, options: .atomic)
      defer { try? manager.removeItem(at: pairingURL) }

      switch request.operation {
      case .prepare:
        let readiness = try retryInstalledCryptex(verifyMounted: {
          try StikJIT.isDDIMounted(pairingFile: pairingURL)
        }) {
          let state = StikJIT.prepareDevice(
            pairingFile: pairingURL, paths: paths,
            progress: { NSLog("[PrimeHackJIT] prepare: %@", String(describing: $0)) })
          if case .preparationFailed(let reason) = state {
            throw NSError(domain: "ICubeJITHelper", code: 3,
                          userInfo: [NSLocalizedDescriptionKey: reason])
          }
          return state
        }
        switch readiness {
        case .ready(let security):
          return .init(success: true,
                       message: "Ready: this device answered over LocalDevVPN and the Developer Disk Image is mounted.",
                       txmPresent: security.isTXMPresent)
        case .unreachable(let reason), .preparationFailed(let reason):
          return .init(success: false, message: reason, txmPresent: StikJIT.isTXMPresent)
        @unknown default:
          return .init(success: false,
                       message: "StikJIT reported a preparation state PrimeHack does not recognise.",
                       txmPresent: StikJIT.isTXMPresent)
        }

      case .enable:
        guard let targetPID = request.targetPID else {
          throw NSError(domain: "ICubeJITHelper", code: 2,
                        userInfo: [NSLocalizedDescriptionKey: "The request is missing the target process ID."])
        }
        // No retry on enable: retrying while another debugger session is active could attach twice.
        try StikJIT.enableJIT(
          targetPID: targetPID,
          pairingFile: pairingURL,
          ddiPaths: paths,
          script: .universal,
          forceScript: true,
          preparationProgress: {
            NSLog("[PrimeHackJIT] prepare: %@", String(describing: $0))
          },
          progress: { NSLog("[PrimeHackJIT] %@", $0) })
        return .init(success: true,
                     message: "PrimeHack detached cleanly from its built-in JIT helper.",
                     txmPresent: StikJIT.isTXMPresent)

      case .resetDDI:
        preconditionFailure("Handled above")
      }
    } catch {
      return .init(success: false, message: error.localizedDescription,
                   txmPresent: StikJIT.isTXMPresent)
    }
  }
}

/// NSExtensionPrincipalClass (Info.plist). Receives a JSON BuiltInJitRequest in the first input
/// item and completes the request with a JSON BuiltInJitRequest.Response.
@objc(ICubeJITHelperHandler)
final class ICubeJITHelperHandler: NSObject, NSExtensionRequestHandling {
  func beginRequest(with context: NSExtensionContext) {
    let info = (context.inputItems.first as? NSExtensionItem)?.userInfo
    let data = info?[BuiltInJitRequest.itemKey] as? Data
    ICubeJITWork.queue.async {
      let response: BuiltInJitRequest.Response
      if let data, let request = try? JSONDecoder().decode(BuiltInJitRequest.self, from: data) {
        response = ICubeJITWork.handle(request)
      } else {
        response = .init(success: false,
                         message: "The JIT helper did not receive a valid request from PrimeHack.",
                         txmPresent: nil)
      }
      let item = NSExtensionItem()
      item.userInfo = [BuiltInJitRequest.Response.itemKey: (try? JSONEncoder().encode(response)) ?? Data()]
      context.completeRequest(returningItems: [item], completionHandler: nil)
    }
  }
}
