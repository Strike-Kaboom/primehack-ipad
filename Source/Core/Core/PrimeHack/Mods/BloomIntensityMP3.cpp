#include "Core/PrimeHack/Mods/BloomIntensityMP3.h"

#include "Core/PrimeHack/PrimeUtils.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"

namespace prime {

void BloomIntensityMP3::replace_start_bloom_func(u32 branch_pos, u32 bloom_func) {
  u32 lis, ori;
  // PrimeHack for iPad: use volatile scratch register r11 instead of callee-saved r16
  // before bloom_func's stack prologue.
  std::tie<u32, u32>(lis, ori) = prime::GetVariableManager()->make_lis_ori(11, "bloom_intensity");
  prime::GetVariableManager()->set_variable(*active_guard, "bloom_intensity", slider_val);
  add_code_change(bloom_func, 0x48000000 | ((branch_pos - bloom_func) & 0x3fffffc));
  add_code_change(branch_pos, lis);                // lis r11, bloom_intensity
  add_code_change(branch_pos + 0x04, ori);         // ori r11, r11, bloom_intensity
  add_code_change(branch_pos + 0x08, 0xc02b0000);  // lfs f1, 0(r11)
  add_code_change(branch_pos + 0x0C, 0x9421fe00);  // stwu	sp, -0x0200 (sp)
  add_code_change(branch_pos + 0x10, 0x48000000 | ((bloom_func - branch_pos - 0xC) & 0x3fffffc));
}

bool BloomIntensityMP3::init_mod(Game game, Region region) {
  if (game != Game::PRIME_3 && game != Game::PRIME_3_STANDALONE) {
    return true;
  }
  prime::GetVariableManager()->register_variable("bloom_intensity");
  slider_val = (GetBloomIntensity() / 100.f);
  switch (game) {
    case Game::PRIME_3:
      if (region == Region::NTSC_U) {
        replace_start_bloom_func(0x80004290, 0x804852cc);
      } else if (region == Region::PAL) {
        replace_start_bloom_func(0x80004290, 0x804849e8);
      }
      break;
    case Game::PRIME_3_STANDALONE:
      if (region == Region::NTSC_U) {
        replace_start_bloom_func(0x80004290, 0x80486880);
      } else if (region == Region::PAL) {
        replace_start_bloom_func(0x80004290, 0x804885a4);
      }
      break;
    default:
      break;
  }
  return true;
}

void BloomIntensityMP3::run_mod(Game game, Region region) {
  switch (game) {
    case Game::PRIME_3:
    case Game::PRIME_3_STANDALONE:
      slider_val = GetBloomIntensity() / 100.f;
      if (prime::GetVariableManager()->get_float(*active_guard, "bloom_intensity") != slider_val) {
        prime::GetVariableManager()->set_variable(*active_guard, "bloom_intensity", slider_val);
      }
      break;
    default:
      break;
  }
}

}  // namespace prime
