#pragma once

#include <stdint.h>
#include <memory>

extern int win_w, win_h;

namespace prime
{

class GenericMouse
{
public:
  virtual ~GenericMouse() = default;
  // Platform dependant implementations are made virtual
  virtual void UpdateInput() = 0;
  virtual void LockCursorToGameWindow() = 0;

  void mousePressEvent(int x, int y);
  void ResetDeltas();
  int32_t GetDeltaVerticalAxis() const;
  int32_t GetDeltaHorizontalAxis() const;

protected:
  // PrimeHack for iPad: initialize fields so NullMouse never returns uninitialized deltas.
  bool cursor_locked = false;
  int32_t dx = 0, dy = 0;
};

class NullMouse : public GenericMouse {
public:
  NullMouse() {}
  void UpdateInput() override {}
  void LockCursorToGameWindow() override {}
};

extern std::unique_ptr<GenericMouse> g_mouse_input;

}  // namespace prime
