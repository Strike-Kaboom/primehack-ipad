#pragma once

#include <concepts>

#include "Core/Core.h"
#include "Core/PrimeHack/PrimeMod.h"

namespace prime {

template <typename T>
concept IsMod = std::derived_from<T, PrimeMod>;

template <IsMod T>
T* GetMod();

template <IsMod T>
void EnableMod(bool notify = true) {
  if (notify) {
    GetMod<T>()->set_state(ModState::ENABLED);
  } else {
    GetMod<T>()->set_state_no_notify(ModState::ENABLED);
  }
}
template <IsMod T>
void DisableMod(bool notify = true) {
  if (notify) {
    GetMod<T>()->set_state(ModState::DISABLED);
  } else {
    GetMod<T>()->set_state_no_notify(ModState::DISABLED);
  }
}
template <IsMod T>
void SetModEnabled(bool enabled) {
  if (enabled) {
    EnableMod<T>();
  } else {
    DisableMod<T>();
  }
}
template <IsMod T>
bool IsModActive() {
  return GetMod<T>()->mod_state() != ModState::DISABLED;
}
template <IsMod T>
void ResetMod() {
  return GetMod<T>()->reset_mod();
}
template <IsMod T>
void EnablePatches() {
  return GetMod<T>()->enable_patches();
}
template <IsMod T>
void DisablePatches() {
  return GetMod<T>()->disable_patches();
}
void RunActiveMods(const Core::CPUThreadGuard& cpu_guard);
Game GetActiveGame();
Region GetActiveRegion();

// PrimeHack for iPad: the player's state, published by the CPU thread every frame for the app
// (DualSense adaptive triggers & lightbar). Safe to read from any thread.
constexpr u32 kPlayerStateInControl = 1u << 0;  // in a Prime game, player loaded, no pause/map/inventory
constexpr u32 kPlayerStateMorphBall = 1u << 1;
constexpr u32 kPlayerStateScanVisor = 1u << 2;
constexpr u32 kPlayerStateHasChargeBeam = 1u << 3;
constexpr u32 kPlayerStateHypermode = 1u << 4;
constexpr u32 kPlayerStateLowHealth = 1u << 5;
constexpr u32 kPlayerStateMissileEmpty = 1u << 6;
constexpr u32 kPlayerStateVisorShift = 8u;
constexpr u32 kPlayerStateVisorMask = 0x7u << kPlayerStateVisorShift;
constexpr u32 kPlayerStateBeamShift = 12u;
constexpr u32 kPlayerStateBeamMask = 0xfu << kPlayerStateBeamShift;
enum class PublishedVisor : u32
{
  Combat = 0,
  Scan = 1,
  Thermal = 2,
  XRay = 3,
  Dark = 4,
  Echo = 5,
  Command = 6,
};
enum class PublishedBeam : u32
{
  Power = 0,
  Ice = 1,
  Wave = 2,
  Plasma = 3,
  Phazon = 4,
  Dark = 5,
  Light = 6,
  Annihilator = 7,
};
u32 GetPublishedPlayerState();
void PublishPlayerState(u32 state);
u32 GetPublishedDamageCounter();
void RecordPlayerDamage();
bool IsPrimeHackEnabledForRun();
void Shutdown(bool reset_run_latch = true);

using GameChangeCallback = std::function<void(Game, Region)>;
void AddOnGameChangeCallback(GameChangeCallback cb);

// Used to make savestates play nicer. Must be called from CPU thread
// Will stash all patches done by primehack, used on savestate write before RAM is saved
void StashMemoryChanges();
// Will restore all stashed patches done by primehack, used on savestate write after RAM is saved
void RestoreMemoryChanges();

bool CachedHardcoreEnabled();

} // namespace prime
