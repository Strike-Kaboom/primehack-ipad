#include "Core/PrimeHack/HackManager.h"

#include <atomic>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/Config/Config.h"
#include "Core/AchievementManager.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/PrimeHack/GuestAllocator.h"
#include "Core/PrimeHack/HackConfig.h"
#include "Core/PrimeHack/PrimeUtils.h"
#include "Core/PrimeHack/Mods/ModListMacro.h"
#include "Core/PrimeHack/Mods/ModHeaders.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "InputCommon/GenericMouse.h"

namespace prime {
namespace {

#define FOURCC(a, b, c, d) (((static_cast<u32>(a) << 24) & 0xff000000) | \
                            ((static_cast<u32>(b) << 16) & 0x00ff0000) | \
                            ((static_cast<u32>(c) << 8) & 0x0000ff00) | \
                            (static_cast<u32>(d) & 0x000000ff))

#define CSEP(x) x,
#define X(x) x
std::tuple<MOD_LIST(CSEP, X)> sModsTuple;
#undef CSEP
#undef X

// Active game tracking globals
// PrimeHack for iPad: atomic so main-thread UpdateHackSettings() does not race with the CPU thread.
std::atomic<Game> sActiveGame{Game::INVALID_GAME};
Game sLastGame = Game::INVALID_GAME;
std::atomic<Region> sActiveRegion{Region::INVALID_REGION};
Region sLastRegion = Region::INVALID_REGION;
std::vector<GameChangeCallback> sGameChangeCbList;

bool sHardcoreEnabled = false;
#ifdef IPHONEOS
// PrimeHack for iPad: latched on game start to match InputConfig::LoadConfig() and the Settings UI
// ("Takes effect when a game starts"), so toggling PrimeHack Controls mid-game cannot desync the
// mods from the active controller mapping.
bool sPrimeHackEnabledForRun = true;
bool sPrimeHackEnabledLatched = false;
#endif
// PrimeHack for iPad: see PublishPlayerState.
std::atomic<u32> sPublishedPlayerState{0};
bool sChangesStashed = false;

// Dynalib tracking globals
struct LoadedRsoModule {
  u32 base = 0;
  u32 old_epilogue = 0;
  std::vector<CodeChange> original_code;
  std::vector<CodeChange> patched_code;
};
std::unordered_map<std::string, LoadedRsoModule> sModuleList;
std::vector<CodeChange> sDynalibOriginal;
std::vector<CodeChange> sDynalibChanges;
u32 sEpilogueHookStub = 0;
constexpr u32 kModuleEpilogueOffset = 0x28;
constexpr u32 kEpilogueStubSize = 0x18;
constexpr std::string_view kEpilogueAsmStub = R"(
.defvar StubBegin, 0x{stub_begin:x}
.defvar VmcallInst, 0x{vmcall_inst:x}

.locate StubBegin
.4byte VmcallInst
# The above vmcall will fix the epilogue address for us
lwz r12, 0x28(r3)
cmpwi r12, 0
beqlr
mtctr r12
bctr
)";

// Updates the sActiveGame and sActiveRegion globals
void update_active_game_region(const Core::CPUThreadGuard& cpu_guard) {
  Game active_game = Game::INVALID_GAME;
  Region active_region = Region::INVALID_REGION;

  // PrimeHack for iPad: check the disc ID at 0x80000000 FIRST. Checking 0x8046d340 unconditionally
  // matched common words like blr (0x4e800020) or MEM1 heap pointers (0x80830000) in non-Prime
  // games and even live data in GameCube Prime 1/2, misidentifying them as Trilogy.
  const u32 disc_id = PowerPC::MMU::HostRead<u32>(cpu_guard, 0x80000000);
  switch (disc_id) {
    case FOURCC('R', '3', 'M', 'E'):
      switch (PowerPC::MMU::HostRead_Instruction(cpu_guard, 0x8046d340)) {
        case 0x38000018:
          active_game = Game::MENU;
          active_region = Region::NTSC_U;
          break;
        case 0x4e800020:
          active_game = Game::PRIME_1;
          active_region = Region::NTSC_U;
          break;
        case 0x4bff64e1:
          active_game = Game::PRIME_2;
          active_region = Region::NTSC_U;
          break;
        case 0x80010070:
          if (PowerPC::MMU::HostRead<u32>(cpu_guard, 0x80576ae8) == 0x7d415378) {
            active_game = Game::PRIME_3;
            active_region = Region::NTSC_U;
          }
          break;
        default:
          break;
      }
      break;
    case FOURCC('R', '3', 'M', 'P'):
      switch (PowerPC::MMU::HostRead_Instruction(cpu_guard, 0x8046d340)) {
        case 0x7c0000d0:
          active_game = Game::MENU;
          active_region = Region::PAL;
          break;
        case 0x7c962378:
          active_game = Game::PRIME_1;
          active_region = Region::PAL;
          break;
        case 0x80830000:
          active_game = Game::PRIME_2;
          active_region = Region::PAL;
          break;
        case 0x3a800000:
          if (PowerPC::MMU::HostRead<u32>(cpu_guard, 0x805795a4) == 0x7d415378) {
            active_game = Game::PRIME_3;
            active_region = Region::PAL;
          }
          break;
        default:
          break;
      }
      break;
    case FOURCC('G', 'M', '8', 'E'):
      active_region = Region::NTSC_U;
      switch (PowerPC::MMU::HostRead<u8>(cpu_guard, 0x80000007)) {
        case 0:
          active_game = Game::PRIME_1_GCN;
          break;
        case 1:
          active_game = Game::PRIME_1_GCN_R1;
          break;
        case 2:
          active_game = Game::PRIME_1_GCN_R2;
          break;
        default:
          active_region = Region::INVALID_REGION;
          break;
      }
      break;
    case FOURCC('G', 'M', '8', 'P'):
      active_game = Game::PRIME_1_GCN;
      active_region = Region::PAL;
      break;
    case FOURCC('G', '2', 'M', 'E'):
      active_game = Game::PRIME_2_GCN;
      active_region = Region::NTSC_U;
      break;
    case FOURCC('G', '2', 'M', 'P'):
      active_game = Game::PRIME_2_GCN;
      active_region = Region::PAL;
      break;
    case FOURCC('R', 'M', '3', 'E'):
      active_game = Game::PRIME_3_STANDALONE;
      active_region = Region::NTSC_U;
      break;
    case FOURCC('R', 'M', '3', 'P'):
      active_game = Game::PRIME_3_STANDALONE;
      active_region = Region::PAL;
      break;
    default:
      break;
  }

  sActiveGame.store(active_game, std::memory_order_relaxed);
  sActiveRegion.store(active_region, std::memory_order_relaxed);
}

void update_hardcore_enabled() {
#ifdef USE_RETRO_ACHIEVEMENTS
  sHardcoreEnabled = AchievementManager::GetInstance().IsHardcoreModeActive();
#endif
}

void update_mod_state_from_config() {
  SetModEnabled<AutoEFB>(UseMPAutoEFB());
  SetModEnabled<CutBeamFxMP1>(GetEnableSecondaryGunFX());
  SetModEnabled<AutoFogToggleMP3>(GetAutoFogToggleEnabled());

#ifdef IPHONEOS
  // PrimeHack for iPad: each of these has its own switch in Settings -> PrimeHack, and the app has no
  // separate "Enable Cheats" master switch for them, so turning one on is enough.
  constexpr bool cheats_allowed = true;
#else
  const bool cheats_allowed = Config::Get(Config::MAIN_ENABLE_CHEATS);
#endif
  if (cheats_allowed) {
    SetModEnabled<Noclip>(Config::Get(Config::PRIMEHACK_NOCLIP));
    SetModEnabled<Invulnerability>(Config::Get(Config::PRIMEHACK_INVULNERABILITY));
    SetModEnabled<SkipCutscene>(Config::Get(Config::PRIMEHACK_SKIPPABLE_CUTSCENES));
    SetModEnabled<RestoreDashing>(Config::Get(Config::PRIMEHACK_RESTORE_SCANDASH));
    SetModEnabled<FriendVouchers>(Config::Get(Config::PRIMEHACK_FRIENDVOUCHERS));
    SetModEnabled<PortalSkipMP2>(Config::Get(Config::PRIMEHACK_SKIPMP2_PORTAL));
    SetModEnabled<DisableHudMemoPopup>(Config::Get(Config::PRIMEHACK_DISABLE_HUDMEMO));
    SetModEnabled<UnlockHypermode>(Config::Get(Config::PRIMEHACK_UNLOCK_HYPERMODE));
    SetModEnabled<AllDoorAnyBeam>(Config::Get(Config::PRIMEHACK_ANYBEAM_DOOR));
  } else {
    DisableMod<Noclip>();
    DisableMod<Invulnerability>();
    DisableMod<SkipCutscene>();
    DisableMod<RestoreDashing>();
    DisableMod<FriendVouchers>();
    DisableMod<PortalSkipMP2>();
    DisableMod<DisableHudMemoPopup>();
    DisableMod<UnlockHypermode>();
    DisableMod<AllDoorAnyBeam>();
  }

  // Disallow any PrimeHack control mods
#ifdef IPHONEOS
  const bool primehack_controls_enabled = sPrimeHackEnabledForRun;
#else
  const bool primehack_controls_enabled = Config::Get(Config::PRIMEHACK_ENABLE);
#endif
  if (!primehack_controls_enabled || UsingRealWiimote()) {
    DisableMod<FpsControls>();
    DisableMod<SpringballButton>();
    DisableMod<ContextSensitiveControls>();
    DisableMod<MapController>();
    return;
  } else {
    EnableMod<FpsControls>();
    EnableMod<SpringballButton>();
    EnableMod<ContextSensitiveControls>();
    if (ImprovedMotionControls()) {
      EnablePatches<ContextSensitiveControls>();
    } else {
      DisablePatches<ContextSensitiveControls>();
    }
    SetModEnabled<MapController>(NewMapControlsEnabled());
  }
}

template <typename Fn>
void foreach_mod(Fn&& fn) {
  std::apply([fn = std::forward<Fn>(fn)](auto&&... args) {
    (fn(args), ...);
  }, sModsTuple);
}

template <typename Mem>
void add_rso(Mem&& mem, u32 module_base, u32 name_base) {
  if (!mem_check_any(module_base) || !mem_check_any(name_base)) {
    return;
  }
  // Add a handler for the epilogue
  u32 old_epilogue_addr = mem.template Read<u32>(module_base + kModuleEpilogueOffset);
  mem.template Write<u32>(sEpilogueHookStub, module_base + kModuleEpilogueOffset);

  std::string module_name = std::filesystem::path(readin_str(mem, name_base)).filename().string();
  LoadedRsoModule mod_entry;
  mod_entry.base = module_base;
  mod_entry.old_epilogue = old_epilogue_addr;

  auto& ppc = Core::System::GetInstance().GetPowerPC();
  foreach_mod([&mem, &ppc, module_base, &module_name, &mod_entry](PrimeMod& mod) {
    if (mod.mod_state() == ModState::DISABLED) {
      return;
    }

    auto const* changes = mod.get_pending_dyna_changes(module_name);
    if (changes == nullptr) {
      return;
    }

    for (auto const& change : *changes) {
      const u32 addr = change.address + module_base;
      const u32 orig = mem.template Read<u32>(addr);
      mod_entry.original_code.emplace_back(addr, orig);
      mod_entry.patched_code.emplace_back(addr, change.var);
      if (orig != change.var) {
        mem.template Write<u32>(change.var, addr);
        ppc.ScheduleInvalidateCacheThreadSafe(addr);
      }
    }
  });
  sModuleList.insert_or_assign(std::move(module_name), std::move(mod_entry));
}

void mp3_post_rso_link(PowerPC::PowerPCState& state, PowerPC::MMU& mmu, u32) {
  // Hook point is directly after RSO prologue is ran. RSO is in r31, filesystem path in r29
  add_rso(mmu, state.gpr[31], state.gpr[29]);
  // Original instruction: lbz r0, 9(r1)
  state.gpr[0] = mmu.Read<u8>(state.gpr[1] + 9);
}

u32 remove_rso(PowerPC::MMU& mmu, u32 mod_base) {
  u32 epilogue = 0;
  std::erase_if(sModuleList, [&epilogue, mod_base](auto const& item) {
    if (item.second.base == mod_base) {
      epilogue = item.second.old_epilogue;
      return true;
    }
    return false;
  });
  return epilogue;
}

void mp3_rso_unlink(PowerPC::PowerPCState& state, PowerPC::MMU& mmu, u32) {
  // Hook point is the prolog itself, so r3 should be a pointer to the module base
  const u32 epilogue = remove_rso(mmu, state.gpr[3]);
  // Write the old epilogue back so that savestating doesn't shit itself
  mmu.Write<u32>(epilogue, state.gpr[3] + kModuleEpilogueOffset);
}

void init_dynalib(Core::CPUThreadGuard const& guard, Game game, Region region) {
  sDynalibChanges.clear();
  sDynalibOriginal.clear();

  using namespace Common::GekkoAssembler;
  if (game != Game::PRIME_3 && game != Game::PRIME_3_STANDALONE) {
    return;
  }

  u32 load_hook = 0;
  if (game == Game::PRIME_3) {
    if (region == Region::NTSC_U) {
      load_hook = 0x8019a30c;
    } else if (region == Region::PAL) {
      load_hook = 0x80199dec;
    }
  } else if (game == Game::PRIME_3_STANDALONE) {
    if (region == Region::NTSC_U) {
      load_hook = 0x8019f8cc;
    } else if (region == Region::PAL) {
      load_hook = 0x801a05dc;
    }
  }

  const int vmc_load_hook_mp3_idx =
    Core::System::GetInstance().GetPowerPC().RegisterVmcall(mp3_post_rso_link);
  const int vmc_unload_hook_mp3_idx =
    Core::System::GetInstance().GetPowerPC().RegisterVmcall(mp3_rso_unlink);
  const u32 unload_vmc = gen_vmcall(vmc_unload_hook_mp3_idx, 0);

  sEpilogueHookStub = GuestAllocAligned(kEpilogueStubSize, 2);
  auto result =
    Assemble(fmt::format(fmt::runtime(kEpilogueAsmStub),
                         fmt::arg("stub_begin", sEpilogueHookStub),
                         fmt::arg("vmcall_inst", unload_vmc)), 0);
  ASSERT(!IsFailure(result));
  std::vector<CodeBlock> const& code_changes_blocks = GetT(result);
  for (auto const& block : code_changes_blocks) {
    for (u32 i = 0; i < block.instructions.size(); i += 4) {
      sDynalibChanges.emplace_back(block.block_address + i,
                                   Common::swap32(&block.instructions[i]));
    }
  }
  sDynalibChanges.emplace_back(load_hook, gen_vmcall(vmc_load_hook_mp3_idx, 0));

  for (CodeChange const& cc : sDynalibChanges) {
    sDynalibOriginal.emplace_back(cc.address, PowerPC::MMU::HostRead<u32>(guard, cc.address));
  }

  // This is exclusively to handle savestate loads more gracefully
  const u32 rso_list_base = GetAddressDB()->lookup_address(game, region, "rso_list_base");
  if (!mem_check_any(rso_list_base)) {
    return;
  }
  constexpr int kMaxRsoEntries = 256;
  int visited = 0;
  for (u32 cur_entry = PowerPC::MMU::HostRead<u32>(guard, rso_list_base + 4);
       visited < kMaxRsoEntries && cur_entry != 0 && cur_entry != rso_list_base &&
       mem_check_any(cur_entry);
       cur_entry = PowerPC::MMU::HostRead<u32>(guard, cur_entry + 4), ++visited) {
    const u32 rso_metadata = PowerPC::MMU::HostRead<u32>(guard, cur_entry + 0xc);
    if (!mem_check_any(rso_metadata)) {
      continue;
    }

    // Presumably the load state of the module, 1 seems to correspond with a loaded & linked mod
    if (PowerPC::MMU::HostRead<u32>(guard, rso_metadata + 0x20) != 1) {
      continue;
    }

    u32 module_base = PowerPC::MMU::HostRead<u32>(guard, rso_metadata + 0x18);
    u32 name_base = PowerPC::MMU::HostRead<u32>(guard, rso_metadata);

    add_rso(HostMem(guard), module_base, name_base);
  }
}

void apply_cc_list(const Core::CPUThreadGuard& cpu_guard, std::vector<CodeChange> const& cc_list) {
  auto& ppc = Core::System::GetInstance().GetPowerPC();
  for (CodeChange const& cc : cc_list) {
    if (PowerPC::MMU::HostRead<u32>(cpu_guard, cc.address) != cc.var) {
      PowerPC::MMU::HostWrite<u32>(cpu_guard, cc.var, cc.address);
      ppc.ScheduleInvalidateCacheThreadSafe(cc.address);
    }
  }
}

#ifdef IPHONEOS
// PrimeHack for iPad (no-JIT speed): R3M.ini turns SyncGPU on for all of Trilogy (a PrimeHack
// addition; upstream Dolphin's R3M has none). It is kept for the Prime 2 part, whose engine upstream
// runs single-core on GameCube for stability; Prime 1 and Prime 3 run with the GPU thread unsynced,
// as upstream does, so the Cached Interpreter is not interrupted every 1000 ticks to wait for it.
// The menu keeps whatever is current. Written to the CurrentRun layer, which ends with the game;
// FifoManager re-reads it through its config callback.
void UpdateTrilogyGpuSync(prime::Game game) {
  if (!SConfig::GetInstance().GetGameID().starts_with("R3M"))
    return;
  if (game == prime::Game::PRIME_2)
    Config::SetCurrent(Config::MAIN_SYNC_GPU, true);
  else if (game == prime::Game::PRIME_1 || game == prime::Game::PRIME_3)
    Config::SetCurrent(Config::MAIN_SYNC_GPU, false);
}
#endif

} // namespace

void RunActiveMods(const Core::CPUThreadGuard& cpu_guard) {
  // When launching a new title, the EH being 0 is a good sign
  // that the game isn't done loading yet
  u32 exception_hook = PowerPC::MMU::HostRead<u32>(cpu_guard, 0x80000048);
  if (exception_hook == 0) {
    return;
  }

  update_active_game_region(cpu_guard);
  const Game active_game = sActiveGame.load(std::memory_order_relaxed);
  const Region active_region = sActiveRegion.load(std::memory_order_relaxed);

#ifdef IPHONEOS
  if (!sPrimeHackEnabledLatched && active_game != Game::INVALID_GAME) {
    sPrimeHackEnabledForRun = Config::Get(Config::PRIMEHACK_ENABLE);
    sPrimeHackEnabledLatched = true;
  }
  EnforceIOSControllerLayout(active_game);
#endif

  // Before doing any mod-related code, set the cpu guard
  foreach_mod([&cpu_guard](PrimeMod& mod) { mod.set_temporary_cpu_guard(&cpu_guard); });

  if (active_game != sLastGame || active_region != sLastRegion) {
    sPublishedPlayerState.store(0, std::memory_order_relaxed);
    UpdateHackSettings();
#ifdef IPHONEOS
    UpdateTrilogyGpuSync(active_game);
#endif
    AllocSwitchGame(active_game, active_region);
    foreach_mod([](PrimeMod& mod) { mod.reset_mod(); });
    GetVariableManager()->reset_variables();
    sDynalibChanges.clear();
    sDynalibOriginal.clear();
    // PrimeHack for iPad: clear MP3 RSO module list on game change so savestates in MP1/MP2
    // cannot write stale MP3 epilogues into guest RAM, and record sLastGame/sLastRegion even
    // when transitioning to INVALID_GAME so the reset block does not re-run every frame.
    sModuleList.clear();
    sLastGame = active_game;
    sLastRegion = active_region;

    for (auto& cb : sGameChangeCbList) {
      cb(active_game, active_region);
    }
    // Cache hardcore state at each game change
    update_hardcore_enabled();
  }

  update_mod_state_from_config();

  if (active_game != Game::INVALID_GAME && active_region != Region::INVALID_REGION) {
    foreach_mod([active_game, active_region](PrimeMod& mod) {
      // Skip any "cheat" mods
      if (mod.is_cheat() && sHardcoreEnabled) {
        return;
      }

      if (!mod.is_initialized() && mod.init_mod(active_game, active_region)) {
        mod.mark_initialized();
      }
      if (mod.should_apply_changes()) {
        mod.update_original_instructions();
        mod.apply_instruction_changes();
      }
    });

    // Initialize the dynalib AFTER mods to capture their requested changes for savestates
    if (sDynalibChanges.empty()) {
      init_dynalib(cpu_guard, active_game, active_region);
    }
    apply_cc_list(cpu_guard, sDynalibChanges);

    sLastGame = active_game;
    sLastRegion = active_region;

    foreach_mod([active_game, active_region](PrimeMod& mod) {
      if (mod.is_cheat() && sHardcoreEnabled) {
        return;
      }

      if (mod.mod_state() == ModState::ENABLED) {
        mod.run_mod(active_game, active_region);
      }
    });
  }

  foreach_mod([](PrimeMod& mod) {
    mod.set_temporary_cpu_guard(nullptr);
  });

  // PrimeHack for iPad: guard against null mouse backend before ControllerInterface initializes.
  if (prime::g_mouse_input)
    prime::g_mouse_input->ResetDeltas();
}

Game GetActiveGame() {
  return sActiveGame.load(std::memory_order_relaxed);
}

u32 GetPublishedPlayerState() {
  return sPublishedPlayerState.load(std::memory_order_relaxed);
}

void PublishPlayerState(u32 state) {
  sPublishedPlayerState.store(state, std::memory_order_relaxed);
}

Region GetActiveRegion() {
  return sActiveRegion.load(std::memory_order_relaxed);
}

bool IsPrimeHackEnabledForRun() {
#ifdef IPHONEOS
  return sPrimeHackEnabledLatched ? sPrimeHackEnabledForRun : Config::Get(Config::PRIMEHACK_ENABLE);
#else
  return Config::Get(Config::PRIMEHACK_ENABLE);
#endif
}

void Shutdown(bool reset_run_latch) {
  // HACK: Called from place that does not provide
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  foreach_mod([&guard](PrimeMod& mod) {
    mod.set_temporary_cpu_guard(&guard);
    mod.reset_mod();
    mod.set_temporary_cpu_guard(nullptr);
  });
  GetVariableManager()->reset_variables();
  sDynalibChanges.clear();
  sDynalibOriginal.clear();
  sModuleList.clear();
  sChangesStashed = false;
#ifdef IPHONEOS
  if (reset_run_latch) {
    sPrimeHackEnabledForRun = true;
    sPrimeHackEnabledLatched = false;
  }
#else
  (void)reset_run_latch;
#endif

  sActiveGame.store(Game::INVALID_GAME, std::memory_order_relaxed);
  sLastGame = Game::INVALID_GAME;
  sPublishedPlayerState.store(0, std::memory_order_relaxed);
  sActiveRegion.store(Region::INVALID_REGION, std::memory_order_relaxed);
  sLastRegion = Region::INVALID_REGION;
  for (auto& cb : sGameChangeCbList) {
    cb(Game::INVALID_GAME, Region::INVALID_REGION);
  }
}

void AddOnGameChangeCallback(GameChangeCallback cb) {
  sGameChangeCbList.emplace_back(std::move(cb));
}

void StashMemoryChanges() {
  if (sChangesStashed) {
    return;
  }
  sChangesStashed = true;
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  foreach_mod([&guard](PrimeMod& mod) {
    mod.set_temporary_cpu_guard(&guard);
    mod.overlay_disable();
    mod.set_temporary_cpu_guard(nullptr);
  });
  apply_cc_list(guard, sDynalibOriginal);
  for (auto const& [name, module] : sModuleList) {
    apply_cc_list(guard, module.original_code);
    if (mem_check_any(module.base)) {
      PowerPC::MMU::HostWrite<u32>(guard, module.old_epilogue,
                                   module.base + kModuleEpilogueOffset);
    }
  }
}

void RestoreMemoryChanges() {
  if (!sChangesStashed) {
    return;
  }
  sChangesStashed = false;
  Core::CPUThreadGuard guard(Core::System::GetInstance());
  foreach_mod([&guard](PrimeMod& mod) {
    mod.set_temporary_cpu_guard(&guard);
    mod.lift_overlay();
    mod.set_temporary_cpu_guard(nullptr);
  });
  apply_cc_list(guard, sDynalibChanges);
  for (auto const& [name, module] : sModuleList) {
    apply_cc_list(guard, module.patched_code);
    if (mem_check_any(module.base)) {
      PowerPC::MMU::HostWrite<u32>(guard, sEpilogueHookStub,
                                   module.base + kModuleEpilogueOffset);
    }
  }
}

bool CachedHardcoreEnabled() {
  return sHardcoreEnabled;
}

// Autogenerate GetMod<ty>
#define GEN_GET_MOD(ty) \
  template <> \
  ty* GetMod() { \
    return &std::get<ty>(sModsTuple); \
  }

MOD_LIST(GEN_GET_MOD, GEN_GET_MOD)

} // namespace prime
