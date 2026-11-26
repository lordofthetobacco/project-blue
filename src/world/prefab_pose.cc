#include "world/prefab_pose.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "prefab/prefab.h"
#include "render/scene_view.h"

namespace blue::world {

PrefabPose::PrefabPose(const PrefabCache& cache, PrefabId prefab)
    : cache_(&cache), prefab_(prefab) {
  const size_t node_count = cache.prefab(prefab).hierarchy.nodes.size();
  local_.resize(node_count);
  world_.resize(node_count);
  ResetToRest();
}

bool PrefabPose::Play(int clip, float start_time, float speed,
                      std::optional<bool> loop) {
  const prefab::Prefab& prefab = cache_->prefab(prefab_);
  if (clip < 0 || static_cast<size_t>(clip) >= prefab.clips.size()) {
    return false;
  }
  const animation::Clip& data = prefab.clips[static_cast<size_t>(clip)];
  clip_ = clip;
  player_ = animation::ClipPlayer(data.duration);
  player_.Play(start_time, speed, loop.value_or(data.loop));
  // Allocates only here, never in Update().
  fired_.clear();
  fired_.reserve(data.events.size());
  include_start_ = true;
  Pose();
  return true;
}

bool PrefabPose::Play(std::string_view clip_name, float start_time, float speed,
                      std::optional<bool> loop) {
  return Play(prefab::FindClip(cache_->prefab(prefab_), clip_name), start_time,
              speed, loop);
}

void PrefabPose::ResetToRest() {
  clip_ = -1;
  player_ = animation::ClipPlayer();
  fired_.clear();
  const animation::Hierarchy& hierarchy = cache_->prefab(prefab_).hierarchy;
  animation::ResetToRest(hierarchy, local_);
  animation::ComputeWorldMatrices(hierarchy, local_, world_);
}

void PrefabPose::SetLocalPose(std::span<const animation::Transform> local) {
  clip_ = -1;
  player_ = animation::ClipPlayer();
  fired_.clear();
  const animation::Hierarchy& hierarchy = cache_->prefab(prefab_).hierarchy;
  animation::ResetToRest(hierarchy, local_);
  const size_t count = std::min(local_.size(), local.size());
  std::copy_n(local.begin(), count, local_.begin());
  animation::ComputeWorldMatrices(hierarchy, local_, world_);
}

void PrefabPose::SetTime(float time) {
  if (clip_ < 0) {
    return;
  }
  const bool was_playing = player_.playing();
  const float speed = player_.speed();
  const bool loop = player_.looping();
  player_.Play(time, speed, loop);
  if (!was_playing) {
    player_.Stop();
  }
  fired_.clear();
  include_start_ = false;
  Pose();
}

bool PrefabPose::Update(float dt) {
  fired_.clear();
  if (clip_ < 0 || !player_.playing()) {
    return false;
  }
  const float before = player_.time();
  player_.Update(dt);
  CollectEvents(before);
  include_start_ = false;
  Pose();
  return true;
}

void PrefabPose::Pose() {
  const prefab::Prefab& prefab = cache_->prefab(prefab_);
  animation::ResetToRest(prefab.hierarchy, local_);
  if (clip_ >= 0) {
    animation::SampleClip(prefab.clips[static_cast<size_t>(clip_)],
                          player_.time(), local_);
  }
  animation::ComputeWorldMatrices(prefab.hierarchy, local_, world_);
}

void PrefabPose::CollectEvents(float from) {
  const animation::Clip& clip =
      cache_->prefab(prefab_).clips[static_cast<size_t>(clip_)];
  const float to = player_.time();
  const bool forward = player_.speed() >= 0.0f;
  // A forward loop that wrapped covers (from, duration] and [0, to].
  const bool wrapped = forward ? to < from : to > from;
  for (const animation::ClipEvent& event : clip.events) {
    const float t = event.time;
    bool crossed = false;
    if (forward) {
      const bool after = include_start_ ? t >= from : t > from;
      crossed = wrapped ? (after || t <= to) : (after && t <= to);
    } else {
      const bool before = include_start_ ? t <= from : t < from;
      crossed = wrapped ? (before || t >= to) : (before && t >= to);
    }
    if (crossed && fired_.size() < fired_.capacity()) {
      fired_.push_back(&event);
    }
  }
}

void PrefabPose::AppendDraws(
    const glm::mat4& root, std::vector<render::DrawItem>& out,
    std::span<const std::span<const render::SubmeshDraw>> submeshes) const {
  const prefab::Prefab& prefab = cache_->prefab(prefab_);
  const PrefabGpu& gpu = cache_->gpu(prefab_);
  for (size_t i = 0; i < prefab.meshes.size(); ++i) {
    const prefab::Mesh& mesh = prefab.meshes[i];
    std::span<const render::SubmeshDraw> draws = gpu.submeshes[i];
    if (i < submeshes.size() && !submeshes[i].empty()) {
      draws = submeshes[i];
    }
    out.push_back({.mesh = gpu.meshes[i],
                   .transform = root * NodeMatrix(mesh.node),
                   .bounds = mesh.bounds,
                   .submeshes = draws});
  }
}

std::optional<glm::mat4> PrefabPose::SocketMatrix(std::string_view name) const {
  const prefab::Prefab& prefab = cache_->prefab(prefab_);
  const int socket = prefab::FindSocket(prefab, name);
  if (socket < 0) {
    return std::nullopt;
  }
  const prefab::Socket& data = prefab.sockets[static_cast<size_t>(socket)];
  return NodeMatrix(data.node) * animation::ToMatrix(data.local);
}

glm::mat4 PrefabPose::NodeMatrix(int node) const {
  if (node < 0 || static_cast<size_t>(node) >= world_.size()) {
    return glm::mat4(1.0f);
  }
  return world_[static_cast<size_t>(node)];
}

}  // namespace blue::world
