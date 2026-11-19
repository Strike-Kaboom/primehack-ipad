#pragma once

#include "Core/PrimeHack/PrimeMod.h"
#include "Core/PrimeHack/HackConfig.h"

namespace prime {

// MOD PURPOSE: Automatically toggle dolphin's fog setting when scan visor is on
class AutoFogToggleMP3 : public PrimeMod {
public:
  void run_mod(Game game, Region region) override;
  bool init_mod(Game game, Region region) override;
  // PrimeHack for iPad: on_state_change receives old_state, so check mod_state() (and on_reset)
  // to hand fog back to the user's own setting when the mod is disabled or the game changes.
  void on_state_change(ModState old_state) override {
    if (mod_state() == ModState::DISABLED)
      release_fog();
  }
  void on_reset() override {
    release_fog();
  }

  bool is_cheat() const override { return false; }
  GEN_NAME(AutoFogToggleMP3)

private:
  // Drops this mod's fog override (the Scan Visor in Prime 3 turns fog off). Without this, leaving
  // Prime 3 inside Trilogy while scanning kept fog off in Prime 1 and 2 for the rest of the run.
  void release_fog();
  bool m_fog_overridden = false;
};

} // namespace prime
