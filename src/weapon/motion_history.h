#ifndef BLUE_WEAPON_MOTION_HISTORY_H_
#define BLUE_WEAPON_MOTION_HISTORY_H_

#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>

namespace blue::weapon {

// What one frame drew of the weapon in hand, for MotionHistory::Advance().
struct MotionFrame {
  int slot = 0;  // The active weapon slot (static_cast<int>(WeaponSlot)).
  // Render target size in pixels: the aspect, hence the projection, follows
  // it.
  int width = 0;
  int height = 0;
  float frame_seconds = 0.0f;  // Duration of the frame.
  glm::mat4 view_projection{1.0f};
  // The transform of each weapon draw item, in draw order. Borrowed for the
  // call.
  std::span<const glm::mat4> transforms;
};

// Last frame's matrices for the items of a MotionFrame.
struct PreviousMotion {
  glm::mat4 view_projection{1.0f};
  // Parallel to MotionFrame::transforms. Valid until the next Advance() or
  // Invalidate().
  std::span<const glm::mat4> transforms;
};

// Remembers the weapon's draw-item transforms and the view-projection of the
// previous frame, the inputs of the weapon motion blur
// (render::DrawItem::previous_transform, render::Camera::
// previous_view_projection), and decides when they are meaningful.
//
// Call Advance() once per frame in which the weapon is drawn, Invalidate()
// in every frame it is not drawn and whenever it jumps (respawn, a reset
// viewmodel, a reload of the weapons). History is invalid -- Advance()
// returns nothing, so the items go out unblurred for that frame and nothing
// streaks across the screen -- on the first frame, after Invalidate(), when
// the slot, the target size or the item count changed since the last
// Advance(), and for a frame of zero, negative or non-finite duration.
//
// Plain data and glm only (no renderer types), so the game and its tests use
// it without a GPU. Keeps its allocations between frames. Copyable and
// movable. Not thread-safe.
class MotionHistory {
 public:
  // Forgets the last frame: the next Advance() returns nothing.
  void Invalidate() { valid_ = false; }

  // Records `frame` as the newest and returns the frame before it when the
  // history is valid for `frame` (see the class comment), else nullopt.
  [[nodiscard]] std::optional<PreviousMotion> Advance(const MotionFrame& frame);

 private:
  bool valid_ = false;
  int slot_ = 0;
  int width_ = 0;
  int height_ = 0;
  glm::mat4 view_projection_{1.0f};
  glm::mat4 previous_view_projection_{1.0f};
  std::vector<glm::mat4> transforms_;           // The newest frame's.
  std::vector<glm::mat4> previous_transforms_;  // The frame before.
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_MOTION_HISTORY_H_
