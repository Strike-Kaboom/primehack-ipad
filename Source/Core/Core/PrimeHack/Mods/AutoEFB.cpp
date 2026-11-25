#include "Core/PrimeHack/Mods/AutoEFB.h"

#include "Core/PrimeHack/PrimeUtils.h"

namespace prime {

void AutoEFB::run_mod(Game game, Region) {
  // PrimeHack for iPad: R3M.ini sets EFBToTextureEnable = False for the whole Trilogy disc (the
  // Scan Visor fallback for Prime 2 and 3 when this mod is off). Trilogy's Prime 1 and its launcher
  // never need EFB copies in RAM, so keep them on the fast texture-only path rather than falling
  // back to the INI value, which made every EFB copy in Prime 1 a readback.
  if (game == Game::PRIME_1 || game == Game::MENU) {
    if (!GetEFBTexture()) {
      SetEFBToTexture(true);
      m_efb_overridden = true;
    }
    return;
  }

  if (game != Game::PRIME_2 &&
      game != Game::PRIME_3 &&
      game != Game::PRIME_3_STANDALONE &&
      game != Game::PRIME_2_GCN) {
    // PrimeHack for iPad: release the override when switching to a game that does not use AutoEFB.
    release_efb();
    return;
  }

  bool should_use = false;

  if (game == Game::PRIME_2) {
    LOOKUP_DYN(active_visor);
    if (active_visor == 0)
      return;
    should_use = read32(active_visor) != 2u;
  } else if (game == Game::PRIME_3 || game == Game::PRIME_3_STANDALONE) {
    LOOKUP_DYN(active_visor);
    if (active_visor == 0)
      return;
    should_use = read32(active_visor) != 1u;
  } else if (game == Game::PRIME_2_GCN) {
    LOOKUP_DYN(player);
    if (player == 0)
      return;
    should_use = !read32(player + 0x3d0);
  }

  if (GetEFBTexture() != should_use) {
    SetEFBToTexture(should_use);
    m_efb_overridden = true;
  }
}

void AutoEFB::release_efb() {
  if (!m_efb_overridden)
    return;
  m_efb_overridden = false;
  ClearEFBOverride();
}

bool AutoEFB::init_mod(Game, Region) {
  return true;
}

} // namespace prime
