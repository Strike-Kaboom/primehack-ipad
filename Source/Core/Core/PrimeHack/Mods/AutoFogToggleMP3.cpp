#include "Core/PrimeHack/PrimeUtils.h"
#include "Core/PrimeHack/Mods/AutoFogToggleMP3.h"
#include "Core/PrimeHack/HackConfig.h"

namespace prime {

void AutoFogToggleMP3::run_mod(Game game, Region) {
  if (game != Game::PRIME_3 && game != Game::PRIME_3_STANDALONE) {
    release_fog();
    return;
  }

  bool should_use = false;

  LOOKUP_DYN(active_visor);
  should_use = read32(active_visor) == 1u;

  if (GetFogDisabled() != should_use) {
    SetFogDisabled(should_use);
    m_fog_overridden = true;
  }
}

void AutoFogToggleMP3::release_fog() {
  if (!m_fog_overridden)
    return;
  m_fog_overridden = false;
  ClearFogOverride();
}

bool AutoFogToggleMP3::init_mod(Game, Region) {
  return true;
}

} // namespace prime
