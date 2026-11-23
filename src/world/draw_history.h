#ifndef BLUE_WORLD_DRAW_HISTORY_H_
#define BLUE_WORLD_DRAW_HISTORY_H_

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>

#include "core/extent.h"
#include "render/scene_view.h"

namespace blue::world {

// Remembers, from one frame to the next, where the draw items and the camera
// were: what the renderer needs to know how things moved on screen
// (render::DrawItem::previous_transform and render::Camera::
// previous_view_projection / previous_valid, read for the motion vectors of
// the temporal scaler), and decides when the previous frame is meaningful.
//
// Each frame: BeginFrame(), then Track() once for every run of draw items
// that can move, then TrackCamera(). A run is a group of items that is
// appended as a whole in the same order every frame (the draws of the
// world, of one dropped weapon) and is named by a key that is unique among
// the runs and changes when the run's make-up does.
//
// An item gets a previous_transform only if its matrix differs from the one
// it had in the previous frame: what stands still costs the renderer
// nothing. A run that is new, was not tracked in the previous frame or
// changed its item count gets none for that frame, so nothing that appears
// streaks across the screen; call Forget() for a run that jumped (a
// teleport) and Cut() when the camera did.
//
// Plain data and glm only, so the game, the editor and their tests use it
// without a GPU. Keeps its allocations between frames. Movable, not
// copyable. Not thread-safe.
class DrawHistory {
 public:
  DrawHistory() = default;
  DrawHistory(const DrawHistory&) = delete;
  DrawHistory& operator=(const DrawHistory&) = delete;
  DrawHistory(DrawHistory&&) noexcept = default;
  DrawHistory& operator=(DrawHistory&&) noexcept = default;

  // Starts a frame. The runs that were not tracked in the frame before are
  // forgotten.
  void BeginFrame();

  // Records the transforms of `draws`, the items of run `key` this frame,
  // and sets previous_transform (with motion_blur false: the world is never
  // blurred) on those that moved since the previous frame, when the run's
  // history is valid (see the class comment). Items that already have a
  // previous_transform keep it. At most once per key and frame.
  void Track(uint64_t key, std::span<render::DrawItem> draws);

  // Forgets run `key`: its next Track() sets nothing.
  void Forget(uint64_t key);

  // Records the camera of this frame, seen on a target of `extent` pixels,
  // and sets its previous_view_projection and previous_valid: valid when
  // the frame before was tracked too, on a target of the same size, and no
  // Cut() came in between. An invalid camera gets its own view-projection
  // as the previous one (no camera motion).
  void TrackCamera(render::Camera& camera, core::Extent2D extent);

  // The camera jumped (scene load, respawn, teleport, another view mode):
  // the next TrackCamera() is not valid.
  void Cut() { camera_valid_ = false; }

  // Forgets everything, as after construction.
  void Clear();

 private:
  struct Run {
    std::vector<glm::mat4> transforms;  // As of `frame`.
    uint64_t frame = 0;                 // The last frame it was tracked in.
  };

  uint64_t frame_ = 0;
  std::unordered_map<uint64_t, Run> runs_;
  bool camera_valid_ = false;
  uint64_t camera_frame_ = 0;  // The last frame TrackCamera() was called in.
  core::Extent2D camera_extent_{};
  glm::mat4 view_projection_{1.0f};
};

}  // namespace blue::world

#endif  // BLUE_WORLD_DRAW_HISTORY_H_
