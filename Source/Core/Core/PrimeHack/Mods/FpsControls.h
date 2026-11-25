#pragma once

#include "Core/PrimeHack/PrimeMod.h"

#include <chrono>

using hr_clock = std::chrono::high_resolution_clock;

namespace prime {

// Primary focus of PrimeHack. Does the following:
//  - Provides full control of camera movement to the device outputting to
//    GetHorizontalAxis and GetVerticalAxis
//  - Locks the reticle center-screen
//  - Unlocks the reticle when needed (MP3 ridley fight, cursor control)
//  - Forces freelook during grapple hook/slide
//  - Forces freelook when on ship (MP3)
//  - Gives control of visor & beam switching to the device outputting to
//    CheckVisorCtl and CheckBeamCtl
//  - Removes arm cannon "damped movement" in MP2 & MP3
class FpsControls : public PrimeMod {
public:
  void run_mod(Game game, Region region) override;
  bool init_mod(Game game, Region region) override;
  // PrimeHack for iPad: clear published/session state on disable/reset.
  void on_state_change(ModState old_state) override;
  void on_reset() override;
  bool is_cheat() const override { return false; }
  GEN_NAME(FpsControls)

private:
  // ------------------------------
  // -----Active Mod Functions-----
  // ------------------------------
  bool in_ridley_fight(Region active_region);
  void update_pitchyaw_locked();
  void calculate_pitch_to_target(float target_pitch);
  float calculate_yaw_vel();
  void handle_beam_visor_switch(std::array<int, 4> const &beams,
                                std::array<std::tuple<int, int>, 4> const& visors);
  void mp3_handle_lasso(u32 grapple_state_addr);

  void run_mod_menu(Game game, Region region);
  void run_mod_mp1(Region region);
  void run_mod_mp2(Region region);
  void run_mod_mp3(Game game, Region region);
  void run_mod_mp1_gc(Region region);
  void run_mod_mp2_gc(Region region);

  void CheckBeamVisorSetting(Game game);

  // MP1 GameCube
  void calculate_pitchyaw_delta();

  // ------------------------
  // -----Init Functions-----
  // ------------------------
  void add_beam_change_code_mp1(u32 start_point);
  void add_beam_change_code_mp2(u32 start_point);
  void add_grapple_slide_code_mp3(u32 start_point);
  void add_grapple_lasso_code_mp3(u32 func1, u32 func2, u32 func3);
  void add_control_state_hook_mp3(u32 start_point, Game game, Region region);

  void init_mod_mp1(Region region);
  void init_mod_mp2(Region region);
  void init_mod_mp3(Game game, Region region);
  void init_mod_mp1_gc(Region region);
  void init_mod_mp1_gc_r1();
  void init_mod_mp1_gc_r2();
  void init_mod_mp2_gc(Region region);
  void init_mod_mp3_standalone(Game game, Region region);

  // Required due to MP3
  bool has_beams;

  // We store our pitch value interally to have full control over it
  float pitch;
  float yaw;

  // For interpolating the camera pitch to centre when in MP3 context sensitive mode.
  float start_pitch;
  int delta = 0;
  bool interpolating = false;

  // Prime 3 Grapple Lasso
  bool grapple_button_state, grapple_swap_axis = false;
  float grapple_hand_pos, grapple_force;

  // Check when to reset the cursor position
  bool menu_open = true;

  // Check if we were in the morph ball state last frame.
  bool was_in_morph_ball = false;

  // Check if we were just in the stupid ridley fight from mp3
  bool was_in_ridley_fight = false;
  bool input_disabled() const;

  // PrimeHack for iPad: init_mod zeroes pitch and yaw, and it runs again after every savestate
  // load (State.cpp resets the mods): Resume Where I Left Off, Quick Load, Undo Load. Adopt the
  // game's own view once before driving the camera, so a loaded state keeps the direction it was
  // saved facing instead of snapping to yaw 0 with a level pitch.
  bool needs_view_sync = true;
  bool needs_beam_visor_sync = true;
  void sync_view_from_game(float game_yaw, u32 pitch_addr);

  // PrimeHack for iPad: Damage Thump tracking, Quick 180° turn, and Turn Acceleration ramp.
  float prev_health = -1.f;
  bool has_prev_health = false;
  bool prev_in_hypermode = false;
  bool freelook_ran_this_frame = false;
  bool prev_quick_turn_held = false;
  int quick_turn_frames = 0;
  float turn_accel_progress = 0.f;
  int turn_accel_sign = 0;
};

} // namespace prime
