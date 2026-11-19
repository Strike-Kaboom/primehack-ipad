#pragma once

#include "Core/PrimeHack/PrimeMod.h"
#include "Core/PrimeHack/HackConfig.h"

namespace prime {

// MOD PURPOSE: Automatically toggle EFB settings based on whether scan visor is active
class AutoEFB : public PrimeMod {
public:
  void run_mod(Game game, Region region) override;
  bool init_mod(Game game, Region region) override;
  // PrimeHack for iPad: release the run-time EFB override when disabled or when the game resets/changes.
  void on_state_change(ModState old_state) override {
    if (mod_state() == ModState::DISABLED)
      release_efb();
  }
  void on_reset() override {
    release_efb();
  }

  bool is_cheat() const override { return false; }
  GEN_NAME(AutoEFB)

private:
  void release_efb();
  bool m_efb_overridden = false;
};

} // namespace prime
