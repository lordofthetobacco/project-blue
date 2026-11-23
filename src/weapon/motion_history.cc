#include "weapon/motion_history.h"

#include <cmath>
#include <utility>

namespace blue::weapon {

std::optional<PreviousMotion> MotionHistory::Advance(const MotionFrame& frame) {
  const bool usable = valid_ && frame.slot == slot_ && frame.width == width_ &&
                      frame.height == height_ &&
                      frame.transforms.size() == transforms_.size() &&
                      frame.frame_seconds > 0.0f &&
                      std::isfinite(frame.frame_seconds);

  // The newest frame becomes the previous one; both vectors keep their
  // capacity.
  std::swap(transforms_, previous_transforms_);
  transforms_.assign(frame.transforms.begin(), frame.transforms.end());
  previous_view_projection_ = view_projection_;
  view_projection_ = frame.view_projection;
  slot_ = frame.slot;
  width_ = frame.width;
  height_ = frame.height;
  valid_ = true;

  if (!usable) {
    return std::nullopt;
  }
  return PreviousMotion{.view_projection = previous_view_projection_,
                        .transforms = previous_transforms_};
}

}  // namespace blue::weapon
