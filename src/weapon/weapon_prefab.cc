#include "weapon/weapon_prefab.h"

#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"

namespace blue::weapon {
namespace {

// Returns the rest-pose model-space matrix of every node of `prefab`.
std::vector<glm::mat4> RestMatrices(const prefab::Prefab& prefab) {
  const animation::Hierarchy& hierarchy = prefab.hierarchy;
  std::vector<animation::Transform> pose(hierarchy.nodes.size());
  animation::ResetToRest(hierarchy, pose);
  std::vector<glm::mat4> world(hierarchy.nodes.size(), glm::mat4(1.0f));
  animation::ComputeWorldMatrices(hierarchy, pose, world);
  return world;
}

// Returns the prefab-space matrix of the socket `name`, or nullopt if the
// prefab has none.
std::optional<glm::mat4> SocketMatrix(const prefab::Prefab& prefab,
                                      std::span<const glm::mat4> rest,
                                      std::string_view name) {
  const int index = prefab::FindSocket(prefab, name);
  if (index < 0) {
    return std::nullopt;
  }
  const prefab::Socket& socket = prefab.sockets[static_cast<size_t>(index)];
  const glm::mat4 local = animation::ToMatrix(socket.local);
  if (socket.node < 0 || static_cast<size_t>(socket.node) >= rest.size()) {
    return local;
  }
  return rest[static_cast<size_t>(socket.node)] * local;
}

glm::vec3 Origin(const glm::mat4& matrix) { return glm::vec3(matrix[3]); }

}  // namespace

core::Result<WeaponGeometry, std::string> GeometryFromSockets(
    const prefab::Prefab& prefab) {
  using GeometryResult = core::Result<WeaponGeometry, std::string>;
  const std::vector<glm::mat4> rest = RestMatrices(prefab);
  const std::optional<glm::mat4> muzzle =
      SocketMatrix(prefab, rest, kMuzzleSocket);
  const std::optional<glm::mat4> sight =
      SocketMatrix(prefab, rest, kSightSocket);
  const std::optional<glm::mat4> grip = SocketMatrix(prefab, rest, kGripSocket);
  for (const auto& [socket, name] :
       {std::pair{&muzzle, kMuzzleSocket}, std::pair{&sight, kSightSocket},
        std::pair{&grip, kGripSocket}}) {
    if (!socket->has_value()) {
      return GeometryResult::Error(
          std::format("weapon prefab has no '{}' socket", name));
    }
  }

  WeaponGeometry geometry;
  geometry.scale = 1.0f;
  geometry.muzzle = Origin(*muzzle);
  geometry.sight_eye_point = Origin(*sight);
  const glm::vec3 axis = glm::mat3(*sight) * glm::vec3(0.0f, 0.0f, 1.0f);
  if (!(glm::length(axis) > 0.0f)) {
    return GeometryResult::Error(
        "the 'sight' socket has a degenerate rotation");
  }
  geometry.sight_axis = glm::normalize(axis);
  geometry.grip = Origin(*grip);
  geometry.optic_lens = geometry.sight_eye_point;
  geometry.optic_lens_radius = 0.0f;
  if (const std::optional<glm::mat4> lens =
          SocketMatrix(prefab, rest, kLensSocket)) {
    geometry.optic_lens = Origin(*lens);
    geometry.optic_lens_radius = glm::length(glm::vec3((*lens)[0]));
  }
  return GeometryResult::Ok(geometry);
}

core::Result<WeaponAnimatorTuning, std::string> AnimatorTuningFromClip(
    const animation::Clip& clip) {
  using TuningResult = core::Result<WeaponAnimatorTuning, std::string>;
  const animation::ClipEvent* shot = animation::FindEvent(clip, kShotEvent);
  const animation::ClipEvent* lock =
      animation::FindEvent(clip, kSlideLockEvent);
  for (const auto& [event, name] :
       {std::pair{shot, kShotEvent}, std::pair{lock, kSlideLockEvent}}) {
    if (event == nullptr) {
      return TuningResult::Error(
          std::format("fire clip '{}' has no '{}' event", clip.name, name));
    }
  }
  WeaponAnimatorTuning tuning;
  tuning.fire_time = shot->time;
  tuning.lock_time = lock->time;
  return TuningResult::Ok(tuning);
}

}  // namespace blue::weapon
