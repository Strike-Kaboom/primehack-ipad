// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-3.0-or-later

import Combine
import Darwin
import Foundation

/// PrimeHack for iPad: orchestrates the built-in "Enable JIT" flow (LocalDevVPN probe ->
/// helper DDI prepare -> helper enable with StikJIT `.universal` -> poll CS_DEBUGGED && P_TRACED
/// -> eager LuckTXM bless & detach on a background queue -> JitManager refresh) and JIT Setup actions.
@MainActor
final class BuiltInJitCoordinator: ObservableObject {
  static let shared = BuiltInJitCoordinator()

  enum VPNStatus: Equatable {
    case unknown
    case checking
    case connected(String)
    case disconnected(String)

    var badge: String {
      switch self {
      case .unknown: return L("Not checked")
      case .checking: return L("Checking…")
      case .connected: return L("Connected")
      case .disconnected: return L("Not connected")
      }
    }

    var description: String {
      switch self {
      case .unknown:
        return L("Tap Check Connection to test whether LocalDevVPN is routing to this iPad.")
      case .checking:
        return L("Checking LocalDevVPN loopback on 10.7.0.1…")
      case .connected(let detail):
        return detail
      case .disconnected(let detail):
        return detail
      }
    }
  }

  /// Classifies common helper transport failures into actionable user guidance.
  private enum ConnectionProblem {
    case vpn
    case pairing

    init?(helperMessage: String) {
      let text = helperMessage.lowercased()
      let has: ([String]) -> Bool = { needles in needles.contains(where: text.contains) }
      if has(["connectionreset", "connection reset", "device refused connection"]) {
        self = .pairing
      } else if has(["connectionrefused", "connection refused", "timedout", "timed out",
                     "networkunreachable", "network unreachable", "hostunreachable", "host unreachable",
                     "no route", "early eof"]) {
        self = .vpn
      } else {
        return nil
      }
    }

    var message: String {
      switch self {
      case .vpn:
        return L("Could not reach this device over LocalDevVPN. Connect LocalDevVPN and try again.")
      case .pairing:
        return L("This device dropped the connection, which usually means it no longer accepts this pairing file. Export a fresh pairing file and re-import it in JIT Setup.")
      }
    }
  }

  @Published private(set) var isBusy = false
  @Published private(set) var isJitActive = false
  @Published private(set) var isGameRunning = false
  @Published private(set) var pairingImported = false
  @Published private(set) var vpnStatus: VPNStatus = .unknown
  @Published private(set) var offerOpenLocalDevVPN = false
  @Published private(set) var statusMessage: String?
  @Published private(set) var errorMessage: String?
  @Published private(set) var setupMessage: String?
  @Published private(set) var setupError: String?

  private init() {
    refreshStatus()
  }

  /// Refreshes live JIT, running-game, and pairing-file state from JitManager and Keychain.
  func refreshStatus() {
    let manager = JitManager.shared()
    manager.recheckIfJitIsAcquired()
    if manager.deviceHasTxm {
      isJitActive = manager.acquiredJit && (manager.txmAuthorized || manager.isTXMRegionAuthorized())
    } else {
      isJitActive = manager.acquiredJit
    }
    isGameRunning = TVEmulationBridge.isRunning()
    pairingImported = BuiltInJitPairingStore.isImported
  }

  /// Runs a fast non-blocking LocalDevVPN loopback probe (10.7.0.1:62078).
  func checkVPN() {
    vpnStatus = .checking
    BuiltInJitLoopbackProbe.check(timeout: 0.4) { [weak self] result in
      guard let self else { return }
      if result.reachable {
        self.vpnStatus = .connected(result.userFacingMessage)
        self.offerOpenLocalDevVPN = false
      } else {
        self.vpnStatus = .disconnected(result.userFacingMessage)
        self.offerOpenLocalDevVPN = true
      }
    }
  }

  /// Opens LocalDevVPN to connect its tunnel and return to PrimeHack via `dolphinios`.
  func openLocalDevVPN() {
    BuiltInJitLoopbackProbe.openLocalDevVPN { [weak self] opened in
      guard let self else { return }
      if !opened {
        let message = L("LocalDevVPN could not be opened. Install LocalDevVPN from the App Store, connect its tunnel, and try again.")
        self.errorMessage = message
        self.setupError = message
      }
    }
  }

  // MARK: - Pairing file management (JIT Setup)

  func importPairingFile(from url: URL) {
    setupError = nil
    setupMessage = nil
    do {
      try BuiltInJitPairingStore.importFile(from: url)
      pairingImported = true
      errorMessage = nil
      setupMessage = L("Pairing file validated and saved to the Keychain.")
    } catch {
      pairingImported = BuiltInJitPairingStore.isImported
      setupError = error.localizedDescription
    }
  }

  func importPairingFileFromDropFolder() {
    setupError = nil
    setupMessage = nil
    do {
      BuiltInJitPairingStore.ensureDropFolderExists()
      let imported = try BuiltInJitPairingStore.importFromDropFolderIfPresent()
      pairingImported = BuiltInJitPairingStore.isImported
      if imported {
        errorMessage = nil
        setupMessage = L("Imported pairingFile.plist from Documents/JIT into the Keychain and removed the copy from Documents.")
      } else {
        setupMessage = L("No pairingFile.plist was found in Documents/JIT.")
      }
    } catch {
      pairingImported = BuiltInJitPairingStore.isImported
      setupError = error.localizedDescription
    }
  }

  func removePairingFile() {
    BuiltInJitPairingStore.remove()
    pairingImported = false
    setupError = nil
    setupMessage = L("Saved pairing file removed from the Keychain.")
  }

  // MARK: - Developer Disk Image management (JIT Setup)

  func prepareDeveloperDiskImage() {
    guard !isBusy else { return }
    setupError = nil
    setupMessage = nil

    if let reason = BuiltInJitHost.unavailableReason {
      setupError = reason
      return
    }
    let pairingData: Data
    do {
      pairingData = try BuiltInJitPairingStore.data()
      pairingImported = true
    } catch {
      pairingImported = BuiltInJitPairingStore.isImported
      setupError = error.localizedDescription
      return
    }

    isBusy = true
    Task { @MainActor in
      defer { isBusy = false }

      setupMessage = L("Checking LocalDevVPN…")
      let probe = await probeLoopback()
      guard probe.reachable else {
        vpnStatus = .disconnected(probe.userFacingMessage)
        offerOpenLocalDevVPN = true
        setupMessage = nil
        setupError = probe.userFacingMessage
        return
      }
      vpnStatus = .connected(probe.userFacingMessage)
      offerOpenLocalDevVPN = false

      setupMessage = L("Preparing Developer Disk Image… The first time needs an internet connection.")
      let result = await sendHelperRequest(.prepare(pairingData: pairingData))
      switch result {
      case .success(let response) where response.success:
        setupError = nil
        setupMessage = response.message
      case .success(let response):
        applySetupHelperFailure(response.message)
      case .failure(let error):
        applySetupHelperFailure(error.localizedDescription)
      }
    }
  }

  func resetDeveloperDiskImage() {
    guard !isBusy else { return }
    setupError = nil
    setupMessage = nil

    if let reason = BuiltInJitHost.unavailableReason {
      setupError = reason
      return
    }

    isBusy = true
    Task { @MainActor in
      defer { isBusy = false }

      setupMessage = L("Clearing cached Developer Disk Image…")
      let result = await sendHelperRequest(.resetDDI)
      switch result {
      case .success(let response) where response.success:
        setupError = nil
        setupMessage = response.message
      case .success(let response):
        setupMessage = nil
        setupError = response.message
      case .failure(let error):
        setupMessage = nil
        setupError = error.localizedDescription
      }
    }
  }

  // MARK: - Enable JIT (eager pre-authorise & detach)

  func enableJIT() {
    guard !isBusy else { return }
    refreshStatus()

    if isJitActive {
      JitManager.shared().markBuiltInJitEnabled(true)
      errorMessage = nil
      offerOpenLocalDevVPN = false
      statusMessage = activeSuccessMessage()
      return
    }

    if let reason = BuiltInJitHost.unavailableReason {
      errorMessage = reason
      statusMessage = nil
      return
    }

    // PrimeHack for iPad: another JIT tool (StikDebug, Xcode) is already attached. Starting the helper
    // now would race it, so leave JIT to that tool; the next game start uses it.
    if JitManager.shared().checkIfDebuggerAttachedNow() {
      errorMessage = L("Another JIT tool (such as StikDebug) is already attached to PrimeHack. Start a game to use it, or close PrimeHack fully and try the built-in button again.")
      statusMessage = nil
      return
    }

    let pairingData: Data
    do {
      pairingData = try BuiltInJitPairingStore.data()
      pairingImported = true
    } catch {
      pairingImported = BuiltInJitPairingStore.isImported
      errorMessage = error.localizedDescription
      statusMessage = nil
      return
    }

    isBusy = true
    errorMessage = nil
    offerOpenLocalDevVPN = false

    Task { @MainActor in
      defer { isBusy = false }

      // 1. LocalDevVPN loopback probe.
      statusMessage = L("Checking LocalDevVPN…")
      let probe = await probeLoopback()
      guard probe.reachable else {
        vpnStatus = .disconnected(probe.userFacingMessage)
        offerOpenLocalDevVPN = true
        statusMessage = nil
        errorMessage = probe.userFacingMessage
        return
      }
      vpnStatus = .connected(probe.userFacingMessage)

      // 2. Helper prepare (Developer Disk Image; first time needs internet).
      statusMessage = L("Preparing Developer Disk Image… (first time needs internet)")
      let prepareResult = await sendHelperRequest(.prepare(pairingData: pairingData))
      switch prepareResult {
      case .success(let response) where response.success:
        break
      case .success(let response):
        applyEnableHelperFailure(response.message)
        return
      case .failure(let error):
        applyEnableHelperFailure(error.localizedDescription)
        return
      }

      // 3. Helper enable with StikJIT's built-in `.universal` script (`forceScript: true`).
      statusMessage = L("Attaching JIT helper…")
      let manager = JitManager.shared()
      manager.clearTXMHandshakeCookie()

      let session = EnableSession()
      BuiltInJitHost.send(.enable(targetPID: getpid(), pairingData: pairingData)) { result in
        session.result = result
      }

      // 4. Wait for CS_DEBUGGED && P_TRACED (poll every 100 ms, up to 30 s).
      var attached = false
      for _ in 0..<300 {
        if let earlyResult = session.result {
          switch earlyResult {
          case .success(let response):
            manager.recheckIfJitIsAcquired()
            if response.success && manager.acquiredJit {
              attached = true
              break
            }
            applyEnableHelperFailure(response.message)
            return
          case .failure(let error):
            applyEnableHelperFailure(error.localizedDescription)
            return
          }
          if attached { break }
        }
        manager.recheckIfJitIsAcquired()
        if manager.acquiredJit && manager.debuggerAttached {
          attached = true
          break
        }
        try? await Task.sleep(nanoseconds: 100_000_000)
      }

      guard attached else {
        BuiltInJitHost.cancelRunningEnable()
        statusMessage = nil
        errorMessage = L("Timed out waiting for the JIT helper to attach. Make sure LocalDevVPN is connected and try again.")
        offerOpenLocalDevVPN = true
        return
      }

      // 5. Eagerly run LuckTXM bless (universal path forced, skipping brk #0x69) + detach
      //    on a background queue so the helper finishes and frees its RAM before any game boots.
      statusMessage = L("Authorising JIT memory…")
      let needsTxm = manager.deviceHasTxm
      if needsTxm && !manager.isTXMRegionAuthorized() {
        manager.beginTXMHandshake()
      }

      let authorized: Bool = await withCheckedContinuation { continuation in
        DispatchQueue.global(qos: .userInitiated).async {
          let ok = JitManager.shared().performBuiltInTXMAuthorizationOnBackgroundQueue()
          continuation.resume(returning: ok)
        }
      }

      if needsTxm {
        manager.noteTXMHandshakeResult(authorized)
      }

      // 6. Give the helper a brief moment to complete its detach (CMD_DETACH -> `D`), then refresh.
      for _ in 0..<30 {
        if session.result != nil || !manager.checkIfDebuggerAttachedNow() {
          break
        }
        try? await Task.sleep(nanoseconds: 100_000_000)
      }

      if authorized {
        manager.markBuiltInJitEnabled(true)
        refreshStatus()
        errorMessage = nil
        offerOpenLocalDevVPN = false
        statusMessage = activeSuccessMessage()
      } else {
        refreshStatus()
        statusMessage = nil
        errorMessage = L("Could not authorise the JIT memory region. Try again, or reset the Developer Disk Image in JIT Setup.")
      }
    }
  }

  // MARK: - Helpers

  private final class EnableSession {
    var result: Result<BuiltInJitRequest.Response, Error>?
  }

  private func activeSuccessMessage() -> String {
    if TVEmulationBridge.isRunning() {
      return L("JIT active. Because a game is running, it will apply from the next game start.")
    }
    return L("JIT active.")
  }

  private func probeLoopback() async -> BuiltInJitLoopbackProbe.Result {
    await withCheckedContinuation { continuation in
      BuiltInJitLoopbackProbe.check(timeout: 0.4) { result in
        continuation.resume(returning: result)
      }
    }
  }

  private func sendHelperRequest(_ request: BuiltInJitRequest) async -> Result<BuiltInJitRequest.Response, Error> {
    await withCheckedContinuation { continuation in
      BuiltInJitHost.send(request) { result in
        continuation.resume(returning: result)
      }
    }
  }

  private func applyEnableHelperFailure(_ raw: String) {
    statusMessage = nil
    if let problem = ConnectionProblem(helperMessage: raw) {
      errorMessage = problem.message
      offerOpenLocalDevVPN = (problem == .vpn)
    } else {
      errorMessage = raw
      offerOpenLocalDevVPN = false
    }
  }

  private func applySetupHelperFailure(_ raw: String) {
    setupMessage = nil
    if let problem = ConnectionProblem(helperMessage: raw) {
      setupError = problem.message
      offerOpenLocalDevVPN = (problem == .vpn)
    } else {
      setupError = raw
      offerOpenLocalDevVPN = false
    }
  }
}
