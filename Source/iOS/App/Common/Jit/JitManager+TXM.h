// Copyright 2026 PrimeHack for iPad
// SPDX-License-Identifier: GPL-3.0-or-later

#import "JitManager.h"

NS_ASSUME_NONNULL_BEGIN

/// PrimeHack for iPad: ObjC++ bridge for eager universal LuckTXM authorisation and broker detach.
@interface JitManager (TXM)

/// True when the LuckTXM executable memory region is already mapped and authorised in this process.
- (bool)isTXMRegionAuthorized;

/// Runs on a background queue after CS_DEBUGGED && P_TRACED are both true:
/// sets JitType::LuckTXM, forces the universal.js brk #0xf00d protocol (skipping the legacy
/// brk #0x69 probe), allocates and blesses the 160 MiB RX/RW region, and detaches universal.js.
/// On non-TXM devices (or if the region is already authorised), detaches universal.js cleanly.
/// Call -[JitManager beginTXMHandshake] on main before invoking, and -[JitManager
/// noteTXMHandshakeResult:] + -[JitManager recheckIfJitIsAcquired] on main afterward.
- (bool)performBuiltInTXMAuthorizationOnBackgroundQueue;

@end

NS_ASSUME_NONNULL_END
