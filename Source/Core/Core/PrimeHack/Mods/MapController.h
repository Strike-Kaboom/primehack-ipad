#pragma once

#include "Core/PrimeHack/PrimeMod.h"
#include "Core/PrimeHack/Quaternion.h"

namespace prime {

// MOD PURPOSE: Control the map screen rotation with mouse cursor movements for prime 1 & 2
class MapController : public PrimeMod {
public:
  void run_mod(Game game, Region region) override;
  bool init_mod(Game game, Region region) override;
  void on_state_change(ModState) override {}
  void on_reset() override {
    frame_dx = 0.f;
    frame_dy = 0.f;
    x_rot = 0.f;
    y_rot = 0.f;
  }
  bool is_cheat() const override { return false; }
  GEN_NAME(MapController)

  void reset_rotation(float horizontal, float vertical);
  quat compute_orientation();
  float get_player_yaw() const;

private:
  void init_mod_mp1_gc(Game game, Region region);
  void init_mod_mp1(Region region);
  void init_mod_mp2_gc(Region region);
  void init_mod_mp2(Region region);

  float frame_dx = 0.f, frame_dy = 0.f;
  float x_rot = 0.f, y_rot = 0.f;
};

} // namespace prime
