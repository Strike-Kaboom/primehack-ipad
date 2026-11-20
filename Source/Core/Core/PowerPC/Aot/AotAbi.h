// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#define PH_RESTRICT __restrict
#else
#define PH_RESTRICT __restrict__
#endif

typedef unsigned char PHU8;
typedef unsigned short PHU16;
typedef unsigned int PHU32;
typedef unsigned long long PHU64;
typedef int PHS32;

typedef struct PHPair
{
  PHU64 ps0, ps1;
} PHPair;

// Only a layout description. The prefix ends after SPRs. The two layouts match PowerPC.h.
#if defined(__x86_64__) || defined(_M_X64)
#define PH_X64_LAYOUT 1
#define PH_CR_OFFSET 160
#define PH_PS_OFFSET 256
#else
#define PH_X64_LAYOUT 0
#define PH_CR_OFFSET 672
#define PH_PS_OFFSET 160
#endif
typedef struct PHState
{
  PHU32 pc, npc;
  void* reserved_stack_pointer;
  PHU8* gather_pipe_ptr;
  PHU8* gather_pipe_base_ptr;
  PHU32 gpr[32];
#if !PH_X64_LAYOUT
  PHPair ps[32];
#endif
  PHU64 cr[8];
  PHU32 msr, fpscr, feature_flags, Exceptions;
  PHS32 downcount;
  PHU8 xer_ca, xer_so_ov;
  PHU8 reserved_state[10];
#if PH_X64_LAYOUT
  PHPair ps[32];
#endif
  PHU32 reserved_sr[16];
  PHU32 spr[1024];
} PHState;

// List used both to validate the framework and to fingerprint the host's actual layout.
#define PH_STATE_FIELDS(X) \
  X(pc, 0, 4) X(npc, 4, 4) X(gpr, 32, 128) X(ps, PH_PS_OFFSET, 512) \
  X(gather_pipe_ptr, 16, 8) X(gather_pipe_base_ptr, 24, 8) X(feature_flags, PH_CR_OFFSET + 72, 4) \
  X(cr, PH_CR_OFFSET, 64) X(msr, PH_CR_OFFSET + 64, 4) X(fpscr, PH_CR_OFFSET + 68, 4) \
  X(Exceptions, PH_CR_OFFSET + 76, 4) X(downcount, PH_CR_OFFSET + 80, 4) \
  X(xer_ca, PH_CR_OFFSET + 84, 1) X(xer_so_ov, PH_CR_OFFSET + 85, 1) X(spr, 832, 4096)

// PrimeHack for iPad: return reasons propagate through the entire native call stack.
enum PHExit { PH_DISPATCH, PH_BRANCH, PH_RETURN, PH_DECLINE };
enum { PH_ASYNC_EXCEPTIONS = 0x105u, PH_PERFMON = 4 };
struct PHAotEntry;
enum { PH_FEATURE_KEYS = 16, PH_CALL_LIMIT = 64 };
typedef struct PHAotHost
{
  void* context;
  const void* const* logical_pages;
  const PHU32* dbat;
  const void* const* physical_pages;
  PHU32 ram_size, exram_size;
  const void* fprf_updates_off;
  PHU32 hardware_ftz, neon, low_dcbz_hack, checked_fp, checked_div;
  const PHU64* generation;
  const struct PHAotEntry* const* bound;  // (call_slot - 1) * PH_FEATURE_KEYS + feature_flags
  void (*fallback)(void*, PHU32 word);
  void (*mftb)(void*, PHU32 word);
  void (*check_gather_pipe)(void*);
  void (*check_exceptions)(void*);
  void (*performance_monitor)(void*, PHU32 cycles, PHU32 loads_stores, PHU32 fpu);
  void (*profile_block)(void*, PHU32 pc, PHU32 cycles);
} PHAotHost;

typedef PHU32 (*PHAotFn)(PHState*, const PHAotHost*, PHU32 entry_index, PHU32 depth);
typedef struct PHAotEntry
{
  PHU32 entry_pc, span_start, word_count, flags;  // flags bit 0: span may use the FPU
  PHU64 hash64;
  PHAotFn fn;
  PHU32 entry_index;  // instruction offset from span_start
  PHU32 call_slot;    // one-based, zero for entries with no native callers
} PHAotEntry;

typedef struct PHAotModule
{
  PHU32 abi_version, entry_count, call_slots;
  const PHAotEntry* entries;
} PHAotModule;

#ifdef __cplusplus
#define PH_CONSTEXPR constexpr
#define PH_LAYOUT_ASSERT(expr) static_assert(expr)
#else
#define PH_CONSTEXPR static inline
#define PH_LAYOUT_ASSERT(expr) _Static_assert(expr, "PHAot layout mismatch")
#endif

// Bump this revision for semantic changes and for every edit to the call signatures.
PH_CONSTEXPR PHU32 PHAbiMix(PHU32 hash, PHU32 value)
{
  return (hash ^ value) * 16777619u;
}
PH_CONSTEXPR PHU32 PHAbiSeed(void)
{
  PHU32 hash = PHAbiMix(2166136261u, 3);
  const PHU32 sizes[] = {sizeof(void*), sizeof(PHAotHost), PH_FEATURE_KEYS,
                         sizeof(PHAotEntry), sizeof(PHAotModule)};
  for (PHU32 i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i)
    hash = PHAbiMix(hash, sizes[i]);
  return hash;
}
PH_CONSTEXPR PHU32 PHAbiLanes(PHU32 hash, PHU32 first, PHU32 second, PHU32 lane_size,
                               PHU32 pair_size, PHU32 cr_fields, PHU32 cr_lane_size)
{
  const PHU32 layout[] = {first, second, lane_size, pair_size, cr_fields, cr_lane_size};
  for (PHU32 i = 0; i < sizeof(layout) / sizeof(*layout); ++i)
    hash = PHAbiMix(hash, layout[i]);
  return hash;
}
PH_CONSTEXPR PHU32 PHAbiVersion(void)
{
  PHU32 hash = PHAbiSeed();
#define PH_HASH(field, offset, size) hash = PHAbiMix(PHAbiMix(hash, offset), size);
  PH_STATE_FIELDS(PH_HASH)
#undef PH_HASH
  return PHAbiLanes(hash, 0, 8, 8, sizeof(PHPair), 0, sizeof(PHU64));
}
// MSVC's C frontend uses the address constant form of offsetof.
#if defined(_MSC_VER) && !defined(__cplusplus) && !defined(__clang__)
#define PH_OFFSET(type, field) ((PHU64)&(((type*)0)->field))
#elif defined(_MSC_VER) && !defined(__clang__)
#include <stddef.h>
#define PH_OFFSET(type, field) offsetof(type, field)
#else
#define PH_OFFSET(type, field) __builtin_offsetof(type, field)
#endif
#define PH_ASSERT(field, offset, size) \
  PH_LAYOUT_ASSERT(PH_OFFSET(PHState, field) == offset && sizeof(((PHState*)0)->field) == size);
PH_STATE_FIELDS(PH_ASSERT)
#undef PH_ASSERT
PH_LAYOUT_ASSERT(sizeof(PHPair) == 16 && PH_OFFSET(PHPair, ps1) == 8);
#undef PH_OFFSET
#undef PH_LAYOUT_ASSERT
#undef PH_CONSTEXPR
#ifdef __cplusplus
extern "C"
#endif
const PHAotModule* PHAot_GetModule(PHU32 abi_version, const PHAotHost* host);
