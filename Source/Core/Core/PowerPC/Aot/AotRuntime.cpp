// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/PowerPC/Aot/AotRuntime.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include "Common/CPUDetect.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/MainSettings.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/CPU.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace PowerPC::Aot
{
namespace
{
// PrimeHack for iPad: Copy State can race shutdown. A snapshot owns only this runtime's counters.
std::mutex s_report_mutex;
std::weak_ptr<Runtime::Counters> s_report;
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
constexpr PHU32 HostVersion()
{
  PHU32 hash = PHAbiSeed();
#define PH_HOST_FIELD(name, offset, size) \
  hash = PHAbiMix(PHAbiMix(hash, static_cast<PHU32>(offsetof(PowerPCState, name))), \
                  static_cast<PHU32>(sizeof(PowerPCState::name)));
  PH_STATE_FIELDS(PH_HOST_FIELD)
#undef PH_HOST_FIELD
  return PHAbiLanes(hash, static_cast<PHU32>(offsetof(PairedSingle, ps0)),
                    static_cast<PHU32>(offsetof(PairedSingle, ps1)),
                    static_cast<PHU32>(sizeof(PairedSingle::ps0)), static_cast<PHU32>(sizeof(PairedSingle)),
                    static_cast<PHU32>(offsetof(ConditionRegister, fields)),
                    static_cast<PHU32>(sizeof(ConditionRegister::fields[0])));
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
static_assert(sizeof(PairedSingle) == sizeof(PHPair) && offsetof(PairedSingle, ps0) == 0 &&
              offsetof(PairedSingle, ps1) == 8 && offsetof(ConditionRegister, fields) == 0);
static_assert(SPR_HID0 == 1008 && UReg_HID0{}.DCE.StartBit() == 14 &&
              GPFifo::GATHER_PIPE_SIZE == 32 && GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS == 0x0c008000 &&
              BAT_MAPPED_BIT == 1 && BAT_RESULT_MASK == 0xfffffff8);
static_assert(NUM_CPU_FEATURE_FLAG_COMBINATIONS == PH_FEATURE_KEYS);
static_assert(PH_PERFMON == static_cast<unsigned>(FEATURE_FLAG_PERFMON));
static_assert(PH_ASYNC_EXCEPTIONS ==
              (EXCEPTION_EXTERNAL_INT | EXCEPTION_DECREMENTER | EXCEPTION_PERFORMANCE_MONITOR));
static_assert(BAT_INDEX_SHIFT == 17 && BAT_WI_BIT == 4 && SPR_LR == 8 && SPR_CTR == 9 &&
              SPR_HID2 == 920 && SPR_GQR0 == 912 && SPR_SRR1 == 27 && SPR_MMCR0 == 952 &&
              SPR_MMCR1 == 956 && EXCEPTION_FPU_UNAVAILABLE == 64 && EXCEPTION_DSI == 8 &&
              EXCEPTION_PROGRAM == 128 && EXCEPTION_ALIGNMENT == 32 && sizeof(bool) == 1);

bool Valid(const PHAotModule* module)
{
  if (!module || module->abi_version != HostVersion() || !module->entries ||
      !module->entry_count || module->call_slots > module->entry_count)
    return false;
  u32 previous = 0;
  for (const auto& e : std::span(module->entries, module->entry_count))
  {
    if (!e.fn || !e.word_count || e.call_slot > module->call_slots || (e.flags & ~1u) || (e.span_start & 3) || e.entry_index >= e.word_count ||
        u64(e.span_start) + u64(e.word_count) * 4 > (1ull << 32) ||
        e.entry_pc != e.span_start + e.entry_index * 4 || e.entry_pc < previous)
      return false;
    previous = e.entry_pc;
  }
  return true;
}
}  // namespace

Runtime::~Runtime()
{
  Shutdown();
}

void Runtime::Init(Core::System& system, ProfileBlock profile)
{
  Shutdown();
  if (!Config::Get(Config::MAIN_CIR_AOT))
    return;
#if defined(__unix__) || defined(__APPLE__)
#if defined(IPHONEOS) || (defined(__APPLE__) && TARGET_OS_IOS)
  const char* path = "@executable_path/Frameworks/PrimeHackAOT.framework/PrimeHackAOT";
  // PrimeHack for iPad: absent is normal. A present framework that fails signing/loading is diagnostic.
  uint32_t length = 0;
  _NSGetExecutablePath(nullptr, &length);
  std::string binary(length, '\0');
  if (length && _NSGetExecutablePath(binary.data(), &length) == 0)
  {
    if (const auto pos = binary.rfind('/'); pos != std::string::npos)
    {
      binary.resize(pos + 1);
      binary += "Frameworks/PrimeHackAOT.framework/PrimeHackAOT";
      if (access(binary.c_str(), F_OK) != 0 && errno == ENOENT)
        return;
      path = binary.c_str();
    }
  }
#else
  const char* path = std::getenv("DOLPHIN_CIR_AOT_MODULE");
  if (!path || !*path)
    return;
#endif
  m_library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!m_library)
  {
    INFO_LOG_FMT(POWERPC, "AOT module {} could not be loaded: {}", path, dlerror());
    return;
  }
  if (HostVersion() != PHAbiVersion())
  {
    WARN_LOG_FMT(POWERPC, "AOT disabled: PowerPCState layout differs from framework ABI");
    Shutdown();
    return;
  }
  auto& memory = system.GetMemory();
  m_system = &system;
  m_stats = std::make_shared<Counters>();
  m_host.context = this;
  m_host.logical_pages = reinterpret_cast<const void* const*>(memory.GetLogicalPageMappingsBase());
  m_host.physical_pages = reinterpret_cast<const void* const*>(memory.GetPhysicalPageMappingsBase());
  m_host.ram_size = memory.GetRamSizeReal();
  m_host.exram_size = memory.GetEXRAM() ? memory.GetExRamSizeReal() : 0;
  m_host.dbat = system.GetMMU().GetDBATTable().data();
  m_host.fprf_updates_off = &g_fprf_updates_off;
  m_host.hardware_ftz = cpu_info.bFlushToZero;
  m_host.generation = &m_generation;
  m_host.fallback = [](void* p, PHU32 word) {
    auto& self = *static_cast<Runtime*>(p);
    self.m_stats->fallbacks.fetch_add(1, std::memory_order_relaxed);
    const UGeckoInstruction inst{word};
    Interpreter::GetInterpreterOp(inst)(self.m_system->GetInterpreter(), inst);
  };
  m_host.mftb = [](void* p, PHU32 word) {
    const UGeckoInstruction inst{word};
    Interpreter::GetInterpreterOp(inst)(static_cast<Runtime*>(p)->m_system->GetInterpreter(), inst);
  };
  m_host.check_gather_pipe = [](void* p) {
    static_cast<Runtime*>(p)->m_system->GetGPFifo().FastCheckGatherPipe();
  };
  m_host.check_exceptions = [](void* p) { static_cast<Runtime*>(p)->m_system->GetPowerPC().CheckExceptions(); };
  m_host.performance_monitor = [](void* p, PHU32 cycles, PHU32 ls, PHU32 fp) {
    UpdatePerformanceMonitor(cycles, ls, fp, static_cast<Runtime*>(p)->m_system->GetPPCState());
  };
  m_host.profile_block = profile;
  auto get_module = reinterpret_cast<decltype(&PHAot_GetModule)>(dlsym(m_library, "PHAot_GetModule"));
  m_module = get_module ? get_module(HostVersion(), &m_host) : nullptr;
  if (!Valid(m_module))
  {
    WARN_LOG_FMT(POWERPC, "AOT disabled: missing export, incompatible ABI or invalid entry table");
    Shutdown();
    return;
  }
  // The translator numbers only addresses with a direct native caller.
  m_bound = std::make_unique<const PHAotEntry*[]>(size_t(m_module->call_slots) * PH_FEATURE_KEYS);
  m_host.bound = m_bound.get();
  m_stats->entries = m_module->entry_count;
  std::lock_guard lock(s_report_mutex);
  s_report = m_stats;
#endif
}

void Runtime::Shutdown()
{
  m_module = nullptr;
#if defined(__unix__) || defined(__APPLE__)
  if (m_library)
    dlclose(std::exchange(m_library, nullptr));
#endif
  m_host = {};
  m_stats.reset();
  m_bound.reset();
  m_system = nullptr;
}

std::span<const PHAotEntry> Runtime::Candidates(u32 pc) const
{
  if (!m_module)
    return {};
  const std::span entries(m_module->entries, m_module->entry_count);
  return std::ranges::equal_range(entries, pc, {}, &PHAotEntry::entry_pc);
}

const PHAotEntry* Runtime::Bind(u32 pc, Common::RangeSet<u32>& ranges,
                                const PPCAnalyst::PPCAnalyzer& analyzer, size_t max_ops)
{
  for (const auto& e : Candidates(pc))
  {
    auto& system = *m_system;
    auto& cpu = system.GetPowerPC();
    // Let the shipped scalar/NEON replay run when its diagnostic option is enabled.
    if (!Eligible() || ((e.flags & 1) && !cpu.GetPPCState().msr.FP))
      continue;
    u64 hash = 0xcbf29ce484222325ull;
    Common::RangeSet<u32> physical;
    std::vector<u32> words;
    bool readable = true;
    for (u32 i = 0; i < e.word_count; ++i)
    {
      const u32 addr = e.span_start + 4 * i;
      // A page-table/TLB remap need not destroy a bound block. Only admit stable mappings.
      const bool stable = !cpu.GetPPCState().msr.IR ||
                          (system.GetMMU().GetIBATTable()[addr >> BAT_INDEX_SHIFT] & BAT_MAPPED_BIT);
      if (!stable || HLE::GetHookByAddress(addr) || cpu.GetBreakPoints().IsAddressBreakPoint(addr))
      {
        readable = false;
        break;
      }
      const auto inst = system.GetMMU().TryReadInstruction(addr);
      if (!inst.valid || inst.physical_address > 0xfffffffbu)
      {
        readable = false;
        break;
      }
      words.push_back(inst.hex);
      for (int shift : {24, 16, 8, 0})
        hash = (hash ^ static_cast<u8>(inst.hex >> shift)) * 0x100000001b3ull;
      physical.insert(inst.physical_address, inst.physical_address + 4);
    }
    if (!readable)
      m_stats->rejected.fetch_add(1, std::memory_order_relaxed);
    else if (hash != e.hash64)
      m_stats->mismatches.fetch_add(1, std::memory_order_relaxed);
    else if (analyzer.SpanHasIdleLoop(e.span_start, words, max_ops))
      m_stats->rejected.fetch_add(1, std::memory_order_relaxed);
    else
    {
      // A cache rebuild can refresh FP exception and memory options without reloading the dylib.
      m_host.neon = Config::Get(Config::MAIN_CIR_PS_NEON);
      m_host.low_dcbz_hack = Config::Get(Config::MAIN_LOW_DCBZ_HACK);
      m_host.checked_fp = Config::Get(Config::MAIN_FLOAT_EXCEPTIONS);
      m_host.checked_div = Config::Get(Config::MAIN_DIVIDE_BY_ZERO_EXCEPTIONS);
      for (const auto& [begin, end] : physical)
        ranges.insert(begin, end);
      return &e;
    }
  }
  return nullptr;
}

bool Runtime::Eligible() const
{
  return Loaded() && !Config::Get(Config::MAIN_CIR_PS_NEON_VALIDATE) &&
         !(m_system->GetPPCState().Exceptions & ~PH_ASYNC_EXCEPTIONS) && !m_system->IsMMUMode() && !m_system->IsPauseOnPanicMode() &&
         !m_system->GetPowerPC().GetMemChecks().HasAny() && !m_system->GetPPCState().m_enable_dcache;
}

void Runtime::Publish(const PHAotEntry& entry, PHU32 flags)
{
  if (entry.call_slot && flags < PH_FEATURE_KEYS)
    m_bound[size_t(entry.call_slot - 1) * PH_FEATURE_KEYS + flags] = &entry;
  m_stats->bound.fetch_add(1, std::memory_order_relaxed);
}

void Runtime::Invalidate(u32 pc, PHU32 flags)
{
  if (!Loaded())
    return;
  ++m_generation;
  if (flags < PH_FEATURE_KEYS)
  {
    for (const auto& entry : Candidates(pc))
    {
      if (entry.call_slot)
        m_bound[size_t(entry.call_slot - 1) * PH_FEATURE_KEYS + flags] = nullptr;
    }
  }
}

u32 Runtime::Execute(PowerPCState& state, const PHAotEntry& entry)
{
  const auto result = entry.fn(reinterpret_cast<PHState*>(&state), &m_host, entry.entry_index, 0);
  if (result != PH_DECLINE)
    m_stats->executions.fetch_add(1, std::memory_order_relaxed);
  return result;
}

std::string StatsReport()
{
  std::lock_guard lock(s_report_mutex);
  auto stats = s_report.lock();
  if (!stats)
    return {};
  return fmt::format("  AOT entries={} binds={} record_executions={} interpreter_calls={} "
                     "hash_mismatches={} rejected_spans={}\n",
                     stats->entries.load(), stats->bound.load(), stats->executions.load(),
                     stats->fallbacks.load(), stats->mismatches.load(), stats->rejected.load());
}
}  // namespace PowerPC::Aot
