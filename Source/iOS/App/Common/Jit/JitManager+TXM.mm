// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-3.0-or-later

#import "JitManager+TXM.h"

#include <mutex>

#import "Common/MemoryUtil.h"
#import "JitManager+Debugger.h"

namespace {
std::mutex s_builtin_txm_mutex;
}

@implementation JitManager (TXM)

- (bool)isTXMRegionAuthorized {
  return Common::IsTXMJITAvailable_LuckTXM();
}

- (bool)performBuiltInTXMAuthorizationOnBackgroundQueue {
  std::lock_guard<std::mutex> lock(s_builtin_txm_mutex);
  // PrimeHack for iPad: eager pre-authorise & detach for the built-in JIT helper.
  if (!self.deviceHasTxm) {
    if ([self checkIfDebuggerAttachedNow]) {
      Common::DetachUniversalTXMBroker_LuckTXM();
    }
    return [self checkIfProcessIsDebugged];
  }

  if (Common::IsTXMJITAvailable_LuckTXM()) {
    if ([self checkIfDebuggerAttachedNow]) {
      Common::DetachUniversalTXMBroker_LuckTXM();
    }
    return true;
  }

  Common::SetForceUniversalTXM_LuckTXM(true);
  Common::SetJitType(Common::JitType::LuckTXM);
  Common::AllocateExecutableMemoryRegion();
  Common::SetForceUniversalTXM_LuckTXM(false);

  const bool authorized = Common::IsTXMAvailable();
  if (!authorized) {
    Common::SetJitType(Common::JitType::LuckNoTXM);
    // If mmap failed before TxmUniversalDetach() ran, still detach universal.js so the
    // helper extension exits cleanly instead of hanging.
    if ([self checkIfDebuggerAttachedNow]) {
      Common::DetachUniversalTXMBroker_LuckTXM();
    }
  }

  return authorized;
}

@end
