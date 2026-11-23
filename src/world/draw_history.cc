#include "world/draw_history.h"

#include <cstddef>

namespace blue::world {

void DrawHistory::BeginFrame() {
  // A run last tracked two frames ago can no longer give a previous
  // transform: drop it, so runs that are gone (a picked-up weapon) do not
  // pile up.
  std::erase_if(runs_, [this](const auto& entry) {
    return entry.second.frame < frame_;
  });
  ++frame_;
}

void DrawHistory::Track(uint64_t key, std::span<render::DrawItem> draws) {
  const auto [found, inserted] = runs_.try_emplace(key);
  Run& run = found->second;
  const bool valid = !inserted && run.frame + 1 == frame_ &&
                     run.transforms.size() == draws.size();
  if (valid) {
    for (size_t i = 0; i < draws.size(); ++i) {
      render::DrawItem& item = draws[i];
      if (!item.previous_transform && item.transform != run.transforms[i]) {
        item.previous_transform = run.transforms[i];
        item.motion_blur = false;
      }
    }
  }
  run.transforms.resize(draws.size());
  for (size_t i = 0; i < draws.size(); ++i) {
    run.transforms[i] = draws[i].transform;
  }
  run.frame = frame_;
}

void DrawHistory::Forget(uint64_t key) { runs_.erase(key); }

void DrawHistory::TrackCamera(render::Camera& camera, core::Extent2D extent) {
  const glm::mat4 view_projection = camera.projection * camera.view;
  const bool valid = camera_valid_ && camera_frame_ + 1 == frame_ &&
                     extent == camera_extent_;
  camera.previous_view_projection = valid ? view_projection_ : view_projection;
  camera.previous_valid = valid;
  view_projection_ = view_projection;
  camera_extent_ = extent;
  camera_frame_ = frame_;
  camera_valid_ = true;
}

void DrawHistory::Clear() {
  runs_.clear();
  camera_valid_ = false;
}

}  // namespace blue::world
