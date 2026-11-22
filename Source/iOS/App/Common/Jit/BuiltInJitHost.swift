// SPDX-License-Identifier: GPL-3.0-or-later
// Madeira Converter Exception: see LICENSE-EXCEPTION.md
// Portions derived from Madeira (https://github.com/willfaust/Madeira).
// Modified for PrimeHack for iPad, 2026-11-22.

import Darwin
import Foundation

/// Private NSExtension API (ExtensionFoundation) used to launch this app's own classic
/// app extension by its runtime bundle ID.
@objc private protocol NSExtensionClassShim {
  @objc(extensionWithIdentifier:error:)
  func `extension`(withIdentifier identifier: String) throws -> AnyObject
}

@objc private protocol NSExtensionShim {
  @objc(beginExtensionRequestWithInputItems:completion:)
  func beginExtensionRequest(withInputItems items: [Any], completion: @escaping (NSUUID?) -> Void)
  @objc(pidForRequestIdentifier:)
  func pid(forRequestIdentifier identifier: NSUUID) -> Int32
  @objc(setRequestCompletionBlock:)
  func setRequestCompletionBlock(_ block: @escaping (NSUUID?, [Any]?) -> Void)
  @objc(setRequestCancellationBlock:)
  func setRequestCancellationBlock(_ block: @escaping (NSUUID?, NSError?) -> Void)
  @objc(setRequestInterruptionBlock:)
  func setRequestInterruptionBlock(_ block: @escaping (NSUUID?) -> Void)
}

/// PrimeHack for iPad: launches PlugIns/iCubeJITHelper.appex (classic NSExtension on
/// com.apple.ar.viewer) by its runtime bundle identifier so sideloaders that rename
/// bundle IDs (e.g. SideStore) still resolve the helper.
///
/// One extension request per BuiltInJitRequest: the request travels as JSON in the input
/// item's userInfo, and the helper completes the request with a JSON response.
@MainActor
enum BuiltInJitHost {
  static let helperFile = "iCubeJITHelper.appex"

  /// Requests that have started and not finished, kept alive until they complete.
  private static var running: [HelperRequest] = []

  /// Coalescing window for overlapping `.enable` requests so only one helper attaches at a time.
  static let enableJoinWindow: TimeInterval = 90

  static var unavailableReason: String? {
    #if !os(iOS) || targetEnvironment(macCatalyst)
    return "Built-in JIT is only available on iPhone and iPad."
    #elseif targetEnvironment(simulator)
    return "Built-in JIT works only on a real device, not in the simulator."
    #else
    guard #available(iOS 26.0, *) else {
      return "iPadOS 26 or later is needed for built-in JIT."
    }
    return nil
    #endif
  }

  static var isAvailable: Bool { unavailableReason == nil }

  /// The helper's bundle ID in this installation, read from its own Info.plist; nil if omitted.
  static var helperIdentifier: String? {
    guard let url = Bundle.main.builtInPlugInsURL?.appendingPathComponent(helperFile) else { return nil }
    return Bundle(url: url)?.bundleIdentifier
  }

  static func send(_ request: BuiltInJitRequest,
                   started: @escaping () -> Void = {},
                   completion: @escaping (Result<BuiltInJitRequest.Response, Error>) -> Void) {
    func fail(_ code: Int, _ message: String) {
      NSLog("[PrimeHackJIT] [jit-helper] %@", message)
      completion(.failure(NSError(domain: "PrimeHackBuiltInJIT", code: code,
                                  userInfo: [NSLocalizedDescriptionKey: message])))
    }
    if let reason = unavailableReason {
      fail(1, reason)
      return
    }
    guard let identifier = helperIdentifier else {
      fail(2, "This copy of PrimeHack has no JIT helper extension. Reinstall PrimeHack and keep app extensions enabled in your sideloader.")
      return
    }
    if NSClassFromString("NSExtension") == nil {
      dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW)
    }
    guard let extensionClass = NSClassFromString("NSExtension") else {
      fail(3, "The JIT helper could not start because iPadOS is missing the extension API it relies on.")
      return
    }
    let factory = unsafeBitCast(extensionClass as AnyObject, to: NSExtensionClassShim.self)
    let found: AnyObject
    do {
      found = try factory.extension(withIdentifier: identifier)
    } catch {
      fail(2, "iPadOS could not find PrimeHack's JIT helper (\(identifier)): \(error.localizedDescription). Reinstall PrimeHack with app extensions enabled to fix this.")
      return
    }
    guard let data = try? JSONEncoder().encode(request) else {
      fail(4, "PrimeHack could not package the request for its JIT helper.")
      return
    }
    start(request.operation, data: data, extension: found, identifier: identifier, started: started, completion: completion)
  }

  /// Whether a new request of `operation` joins a running Enable of age `runningAge`.
  static func joins(_ operation: BuiltInJitRequest.Operation, runningAge: TimeInterval?, traced: Bool) -> Bool {
    guard operation == .enable, let age = runningAge else { return false }
    return traced || age < enableJoinWindow
  }

  private static func start(_ operation: BuiltInJitRequest.Operation, data: Data,
                            extension found: AnyObject, identifier: String,
                            started: @escaping () -> Void,
                            completion: @escaping (Result<BuiltInJitRequest.Response, Error>) -> Void) {
    if let current = running.last(where: { $0.isOpenEnable }),
       joins(operation, runningAge: current.age, traced: JitManager.shared().checkIfDebuggerAttachedNow()) {
      NSLog("[PrimeHackJIT] [jit-helper] enable already running (%d s); joining it", Int(current.age))
      current.join(started: started, completion: completion)
      return
    }
    let helper = HelperRequest(extension: found, operation: operation.rawValue, completion: completion)
    running.append(helper)
    helper.begin(data: data, identifier: identifier, started: started)
  }

  fileprivate static func finished(_ helper: HelperRequest) {
    running.removeAll { $0 === helper }
  }
}

/// One in-flight request to the JIT helper extension.
@MainActor
private final class HelperRequest {
  private let shim: NSExtensionShim
  private let `extension`: AnyObject
  private let operation: String
  private var completion: ((Result<BuiltInJitRequest.Response, Error>) -> Void)?
  private let begunAt = Date()
  private var hasStarted = false
  private var joinedStarts: [() -> Void] = []
  private var joinedCompletions: [(Result<BuiltInJitRequest.Response, Error>) -> Void] = []

  var isOpenEnable: Bool { operation == BuiltInJitRequest.Operation.enable.rawValue && completion != nil }
  var age: TimeInterval { Date().timeIntervalSince(begunAt) }

  func join(started: @escaping () -> Void, completion: @escaping (Result<BuiltInJitRequest.Response, Error>) -> Void) {
    joinedCompletions.append(completion)
    if hasStarted { started() } else { joinedStarts.append(started) }
  }

  init(extension found: AnyObject, operation: String,
       completion: @escaping (Result<BuiltInJitRequest.Response, Error>) -> Void) {
    self.extension = found
    self.shim = unsafeBitCast(found, to: NSExtensionShim.self)
    self.operation = operation
    self.completion = completion
  }

  func begin(data: Data, identifier: String, started: @escaping () -> Void) {
    shim.setRequestCompletionBlock { [weak self] _, items in
      let info = (items?.first as? NSExtensionItem)?.userInfo
      let payload = info?[BuiltInJitRequest.Response.itemKey] as? Data
      DispatchQueue.main.async { MainActor.assumeIsolated { self?.returned(payload) } }
    }
    shim.setRequestCancellationBlock { [weak self] _, error in
      let message = error?.localizedDescription ?? "no reason given"
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          self?.end(.failure(Self.error(5, "The JIT helper's request was cancelled: \(message)")))
        }
      }
    }
    shim.setRequestInterruptionBlock { [weak self] _ in
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          self?.end(.failure(Self.error(6, "The JIT helper stopped unexpectedly. Give it another try.")))
        }
      }
    }
    let item = NSExtensionItem()
    item.userInfo = [BuiltInJitRequest.itemKey: data]
    shim.beginExtensionRequest(withInputItems: [item]) { [weak self] uuid in
      DispatchQueue.main.async {
        MainActor.assumeIsolated {
          guard let self else { return }
          guard let uuid else {
            self.end(.failure(Self.error(7, "The JIT helper could not be started. Reinstall PrimeHack and keep app extensions enabled in your sideloader.")))
            return
          }
          NSLog("[PrimeHackJIT] [jit-helper] %@: started %@ as pid %d",
                self.operation, identifier, self.shim.pid(forRequestIdentifier: uuid))
          self.hasStarted = true
          started()
          let joined = self.joinedStarts
          self.joinedStarts = []
          joined.forEach { $0() }
        }
      }
    }
  }

  private func returned(_ payload: Data?) {
    guard let payload, let response = try? JSONDecoder().decode(BuiltInJitRequest.Response.self, from: payload) else {
      end(.failure(Self.error(8, "The JIT helper ended without sending a result.")))
      return
    }
    NSLog("[PrimeHackJIT] [jit-helper] %@: finished success=%d", operation, response.success ? 1 : 0)
    end(.success(response))
  }

  private func end(_ result: Result<BuiltInJitRequest.Response, Error>) {
    guard let completion else { return }
    self.completion = nil
    if case .failure(let error) = result {
      NSLog("[PrimeHackJIT] [jit-helper] %@ failed: %@", operation, error.localizedDescription)
    }
    BuiltInJitHost.finished(self)
    let joined = joinedCompletions
    joinedCompletions = []
    completion(result)
    joined.forEach { $0(result) }
  }

  private static func error(_ code: Int, _ message: String) -> NSError {
    NSError(domain: "PrimeHackBuiltInJIT", code: code, userInfo: [NSLocalizedDescriptionKey: message])
  }
}
