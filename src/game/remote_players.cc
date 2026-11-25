#include "game/remote_players.h"

#include <algorithm>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "game/character_animator.h"
#include "physics/character.h"
#include "player/player_controller.h"

namespace blue::game {
namespace {

// Most players a game has (net::kMaxPlayers).
constexpr size_t kReservedPlayers = 64;

}  // namespace

RemotePlayers::RemotePlayers(const physics::CharacterDesc& character,
                             const player::PlayerTuning& tuning)
    : character_(character), tuning_(tuning) {
  placements_.reserve(kReservedPlayers);
  animators_.resize(kReservedPlayers);
  seen_.resize(kReservedPlayers, false);
}

void RemotePlayers::SetRig(std::optional<CharacterRig> rig) {
  rig_ = std::move(rig);
  Clear();
}

void RemotePlayers::Clear() {
  placements_.clear();
  for (std::optional<CharacterAnimator>& animator : animators_) {
    animator.reset();
  }
}

void RemotePlayers::Update(std::span<const RemotePlayerView> players,
                           float dt) {
  constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
  constexpr glm::vec3 kRight{1.0f, 0.0f, 0.0f};
  placements_.clear();
  std::fill(seen_.begin(), seen_.end(), false);
  for (const RemotePlayerView& player : players) {
    if (!player.alive) {
      continue;
    }
    const glm::mat4 turned = glm::rotate(
        glm::translate(glm::mat4(1.0f), player.feet), player.yaw, kUp);
    const glm::vec3 eye =
        player.feet + glm::vec3(0.0f, eye_height(player.crouching), 0.0f);
    // The view frame: yaw about +y, then pitch about the turned +x.
    const glm::mat4 view = glm::rotate(
        glm::rotate(glm::translate(glm::mat4(1.0f), eye), player.yaw, kUp),
        player.pitch, kRight);
    // The prefab's +z forward turned onto the view's -z.
    const glm::mat4 weapon_root =
        glm::rotate(glm::translate(view, kRemoteHandOffset),
                    std::numbers::pi_v<float>, kUp);
    RemotePlayerPlacement placement{
        .id = player.id,
        .crouching = player.crouching,
        .body = turned,
        .weapon = player.weapon,
        .weapon_root = weapon_root,
        .name_anchor =
            player.feet +
            glm::vec3(0.0f, height(player.crouching) + kNameTagLift, 0.0f)};
    if (rig_ && player.id < animators_.size()) {
      std::optional<CharacterAnimator>& animator = animators_[player.id];
      if (!animator) {
        animator.emplace(*rig_);
      }
      seen_[player.id] = true;
      animator->Update({.dt = dt,
                        .feet = player.feet,
                        .velocity = player.velocity,
                        .yaw = player.yaw,
                        .pitch = player.pitch,
                        .crouching = player.crouching,
                        .on_ground = player.on_ground,
                        .aiming = player.aiming,
                        .alive = true,
                        .weapon = player.hold});
      placement.animator = &*animator;
      if (animator->weapon()) {
        placement.weapon_root = animator->root() * *animator->weapon();
      }
      placement.name_anchor = glm::vec3(
          animator->root() *
          glm::vec4(animator->head_top() + glm::vec3(0.0f, kNameTagLift, 0.0f),
                    1.0f));
    }
    placements_.push_back(placement);
  }
  // Players not shown this frame (dead, gone) snap when they are back.
  for (size_t id = 0; id < animators_.size(); ++id) {
    if (!seen_[id] && animators_[id]) {
      animators_[id]->Reset();
    }
  }
}

}  // namespace blue::game
