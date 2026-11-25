// The player model's procedural animation (game::CharacterAnimator) on the
// real rig: kyle.pbo as blue-pack samples cooks it into the build's assets
// (target blue_sample_assets runs before the tests), holding the sample
// M16 and Glock 17.

#include "game/character_animator.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "core/path.h"
#include "game/remote_players.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "weapon/weapon_def.h"

namespace blue::game {
namespace {

constexpr float kDt = 1.0f / 60.0f;

std::string Asset(std::string_view name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

glm::vec3 Apply(const glm::mat4& m, glm::vec3 point) {
  return glm::vec3(m * glm::vec4(point, 1.0f));
}

const CharacterRig& Rig() {
  static const CharacterRig* const rig = [] {
    auto loaded = prefab::LoadPrefab(Asset("kyle.pbo"));
    REQUIRE(loaded.ok());
    auto made =
        MakeCharacterRig(loaded.value().hierarchy, loaded.value().sockets);
    REQUIRE(made.ok());
    return new CharacterRig(std::move(made).value());
  }();
  return *rig;
}

WeaponHold Hold(HeldWeapon kind, std::string_view file) {
  auto loaded = prefab::LoadPrefab(Asset(file));
  REQUIRE(loaded.ok());
  return MakeWeaponHold(kind, loaded.value());
}

bool Finite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool AllFinite(const CharacterAnimator& animator) {
  for (const animation::Transform& local : animator.locals()) {
    if (!Finite(local.translation) || !Finite(local.scale) ||
        !std::isfinite(local.rotation.w) || !std::isfinite(local.rotation.x) ||
        !std::isfinite(local.rotation.y) || !std::isfinite(local.rotation.z)) {
      return false;
    }
  }
  for (int j = 0; j < kJointCount; ++j) {
    if (!Finite(animator.joint_position(static_cast<Joint>(j)))) {
      return false;
    }
  }
  if (animator.weapon()) {
    for (int c = 0; c < 4; ++c) {
      if (!Finite(glm::vec3((*animator.weapon())[c]))) {
        return false;
      }
    }
  }
  return Finite(glm::vec3(animator.root()[3]));
}

TEST_CASE("Two-bone IK reaches reachable targets and extends towards others") {
  const glm::vec3 root(0.1f, 1.2f, -0.3f);
  const float upper = 0.3f;
  const float lower = 0.25f;
  const glm::vec3 pole(0.0f, 0.0f, -1.0f);
  for (const glm::vec3 offset :
       {glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.2f, -0.2f, -0.3f),
        glm::vec3(0.0f, 0.06f, 0.0f), glm::vec3(-0.3f, 0.3f, 0.2f)}) {
    const TwoBoneIk ik =
        SolveTwoBoneIk(root, root + offset, upper, lower, pole);
    CHECK(glm::length(ik.end - (root + offset)) < 1.0e-3f);
    CHECK(glm::length(ik.middle - root) ==
          doctest::Approx(upper).epsilon(1e-4));
    CHECK(glm::length(ik.end - ik.middle) ==
          doctest::Approx(lower).epsilon(1e-3));
  }
  // The middle joint bends towards the pole.
  const TwoBoneIk bent =
      SolveTwoBoneIk(root, root + glm::vec3(0, -0.4f, 0), upper, lower, pole);
  CHECK(bent.middle.z < root.z);
  // Out of reach: fully extended, pointing at the target.
  const glm::vec3 far = root + glm::vec3(1.0f, -1.0f, 0.5f);
  const TwoBoneIk extended = SolveTwoBoneIk(root, far, upper, lower, pole);
  const glm::vec3 direction = glm::normalize(far - root);
  CHECK(glm::length(extended.end - (root + direction * (upper + lower))) <
        1.0e-4f);
  CHECK(glm::length(extended.middle - (root + direction * upper)) < 1.0e-3f);
  // Degenerate input stays finite.
  for (const TwoBoneIk& ik :
       {SolveTwoBoneIk(root, root, upper, lower, pole),
        SolveTwoBoneIk(root, far, 0.0f, 0.0f, glm::vec3(0.0f)),
        SolveTwoBoneIk(root, far, upper, lower, direction),
        SolveTwoBoneIk(root, glm::vec3(std::numeric_limits<float>::quiet_NaN()),
                       upper, lower, pole)}) {
    CHECK(Finite(ik.middle));
    CHECK(Finite(ik.end));
  }
}

TEST_CASE("The rig of kyle.pbo has its bones, palms and 1.8 m height") {
  const CharacterRig& rig = Rig();
  CHECK(rig.node_count == rig.rest.size());
  for (int j = 1; j < kJointCount; ++j) {
    CHECK(glm::length(rig.rest_offset[static_cast<size_t>(j)]) > 0.0f);
  }
  const glm::vec3 head =
      rig.rest_position[static_cast<size_t>(Joint::kHead)] + rig.head_top;
  CHECK(head.y == doctest::Approx(1.8f).epsilon(0.01));
  // Right is +X.
  CHECK(rig.rest_position[static_cast<size_t>(Joint::kRightWrist)].x > 0.3f);
  CHECK(rig.rest_position[static_cast<size_t>(Joint::kLeftWrist)].x < -0.3f);
  // A hierarchy without the bones is refused.
  CHECK(!MakeCharacterRig(animation::Hierarchy{}, {}).ok());
}

// Walks at `speed` m/s in direction `heading` (character space, x/z) and
// checks the stance foot stays put in the world.
void CheckNoFootSlide(float speed, glm::vec3 heading, float yaw) {
  CharacterAnimator animator(Rig());
  const glm::mat4 turn = CharacterRoot(glm::vec3(0.0f), yaw);
  const glm::vec3 velocity =
      glm::vec3(turn * glm::vec4(glm::normalize(heading), 0.0f)) * speed;
  CharacterInput input{.velocity = velocity, .yaw = yaw};
  glm::vec3 feet(0.0f);
  // Two seconds to settle every blend.
  for (int frame = 0; frame < 120; ++frame) {
    feet += velocity * kDt;
    input.feet = feet;
    input.dt = kDt;
    animator.Update(input);
  }
  std::array<std::optional<glm::vec3>, 2> anchor;
  std::array<int, 2> planted_frames{};
  float worst = 0.0f;
  for (int frame = 0; frame < 240; ++frame) {
    feet += velocity * kDt;
    input.feet = feet;
    input.dt = kDt;
    animator.Update(input);
    for (const Side side : {Side::kLeft, Side::kRight}) {
      const size_t s = static_cast<size_t>(side);
      const Joint ankle =
          side == Side::kLeft ? Joint::kLeftAnkle : Joint::kRightAnkle;
      const glm::vec3 world =
          Apply(animator.root(), animator.joint_position(ankle));
      if (!animator.foot_planted(side)) {
        anchor[s].reset();
        continue;
      }
      ++planted_frames[s];
      if (!anchor[s]) {
        anchor[s] = world;
      }
      worst = std::max(worst, glm::length(world - *anchor[s]));
    }
  }
  INFO("speed ", speed, " heading ", heading.x, " ", heading.z);
  CHECK(planted_frames[0] > 20);
  CHECK(planted_frames[1] > 20);
  CHECK(worst < 0.005f);
}

TEST_CASE("Stance feet do not slide at walking and sprinting speeds") {
  for (const float speed : {2.0f, 3.5f, 5.0f, 7.0f}) {
    CheckNoFootSlide(speed, glm::vec3(0.0f, 0.0f, -1.0f), 0.0f);
  }
  CheckNoFootSlide(3.0f, glm::vec3(0.0f, 0.0f, 1.0f), 0.7f);   // backwards
  CheckNoFootSlide(3.0f, glm::vec3(1.0f, 0.0f, 0.0f), -1.2f);  // strafing
  CheckNoFootSlide(4.0f, glm::vec3(-1.0f, 0.0f, -1.0f), 2.5f);
}

TEST_CASE("Feet never go below the ground") {
  const CharacterRig& rig = Rig();
  const float ankle_floor =
      rig.rest_position[static_cast<size_t>(Joint::kLeftAnkle)].y - 1.0e-3f;
  const float toe_floor =
      rig.rest_position[static_cast<size_t>(Joint::kLeftToe)].y - 1.0e-3f;
  CharacterAnimator animator(rig);
  CharacterInput input{.dt = kDt};
  for (int frame = 0; frame < 900; ++frame) {
    const float t = static_cast<float>(frame) * kDt;
    input.velocity = glm::vec3(std::sin(t) * 6.0f, 0.0f, -std::cos(t) * 5.0f);
    input.crouching = (frame / 120) % 2 == 1;
    input.on_ground = (frame % 200) < 170;
    input.yaw = 0.3f * t;
    animator.Update(input);
    for (const Joint ankle : {Joint::kLeftAnkle, Joint::kRightAnkle}) {
      CHECK(animator.joint_position(ankle).y >= ankle_floor);
    }
    if (input.on_ground) {
      for (const Joint toe : {Joint::kLeftToe, Joint::kRightToe}) {
        CHECK(animator.joint_position(toe).y >= toe_floor);
      }
    }
  }
}

TEST_CASE("Crouching lowers the hips and keeps the head under 1.3 m") {
  const CharacterRig& rig = Rig();
  CharacterAnimator animator(rig);
  CharacterInput input{.dt = kDt, .crouching = true};
  for (int frame = 0; frame < 60; ++frame) {
    animator.Update(input);
  }
  const float rest_hip = rig.rest_position[0].y;
  CHECK(animator.joint_position(Joint::kRoot).y < 0.6f * rest_hip);
  CHECK(animator.joint_position(Joint::kRoot).y > 0.4f * rest_hip);
  CHECK(animator.head_top().y < 1.3f);
  input.crouching = false;
  for (int frame = 0; frame < 60; ++frame) {
    animator.Update(input);
  }
  CHECK(animator.joint_position(Joint::kRoot).y > 0.97f * rest_hip);
  CHECK(animator.head_top().y > 1.7f);
}

TEST_CASE("The hands stay on the weapon over the pitch range") {
  const std::pair<HeldWeapon, const char*> weapons[] = {
      {HeldWeapon::kRifle, "m16.pbo"}, {HeldWeapon::kPistol, "glock17.pbo"}};
  for (const auto& [kind, file] : weapons) {
    const WeaponHold hold = Hold(kind, file);
    for (const bool aiming : {false, true}) {
      for (const bool crouching : {false, true}) {
        for (float degrees = -60.0f; degrees <= 60.0f; degrees += 15.0f) {
          CharacterAnimator animator(Rig());
          const CharacterInput input{.dt = kDt,
                                     .pitch = glm::radians(degrees),
                                     .crouching = crouching,
                                     .aiming = aiming,
                                     .weapon = hold};
          for (int frame = 0; frame < 60; ++frame) {
            animator.Update(input);
          }
          REQUIRE(animator.weapon());
          const glm::vec3 grip = Apply(*animator.weapon(), hold.grip);
          const glm::vec3 support = Apply(*animator.weapon(), hold.support);
          INFO(std::string(file), " aiming ", aiming, " crouching ", crouching,
               " pitch ", degrees);
          CHECK(glm::length(animator.palm_position(Side::kRight) - grip) <
                0.02f);
          CHECK(glm::length(animator.palm_position(Side::kLeft) - support) <
                0.02f);
          // The weapon points along the look when aimed.
          if (aiming) {
            const glm::vec3 forward = glm::normalize(
                glm::vec3(*animator.weapon() * glm::vec4(hold.forward, 0.0f)));
            const glm::vec3 look(0.0f, std::sin(glm::radians(degrees)),
                                 -std::cos(glm::radians(degrees)));
            CHECK(glm::dot(forward, look) > 0.999f);
          }
        }
      }
    }
  }
}

// The direction of look angles (camera::ForwardFrom()): yaw 0 looks along
// -Z, positive pitch up.
glm::vec3 LookDirection(float yaw, float pitch) {
  return {-std::sin(yaw) * std::cos(pitch), std::sin(pitch),
          -std::cos(yaw) * std::cos(pitch)};
}

TEST_CASE("The weapon points along the look, in front of the face") {
  auto kyle = prefab::LoadPrefab(Asset("kyle.pbo"));
  REQUIRE(kyle.ok());
  // The face: the front of the head part, relative to the head joint.
  const int head_node = Rig().nodes[static_cast<size_t>(Joint::kHead)];
  float face = 0.0f;
  for (const prefab::Mesh& mesh : kyle.value().meshes) {
    if (mesh.node == head_node) {
      face = mesh.bounds.min.z;
    }
  }
  REQUIRE(face < -0.05f);
  const std::pair<HeldWeapon, const char*> weapons[] = {
      {HeldWeapon::kRifle, "m16.pbo"}, {HeldWeapon::kPistol, "glock17.pbo"}};
  for (const auto& [kind, file] : weapons) {
    auto weapon = prefab::LoadPrefab(Asset(file));
    REQUIRE(weapon.ok());
    const WeaponHold hold = MakeWeaponHold(kind, weapon.value());
    const int muzzle_socket = prefab::FindSocket(weapon.value(), "muzzle");
    REQUIRE(muzzle_socket >= 0);
    const glm::vec3 muzzle_point =
        weapon.value()
            .sockets[static_cast<size_t>(muzzle_socket)]
            .local.translation;
    for (const float yaw : {0.0f, 1.0f, -2.5f}) {
      for (const bool aiming : {false, true}) {
        for (const bool crouching : {false, true}) {
          for (float degrees = -60.0f; degrees <= 60.0f; degrees += 15.0f) {
            const float pitch = glm::radians(degrees);
            CharacterAnimator animator(Rig());
            const CharacterInput input{.dt = kDt,
                                       .feet = glm::vec3(3.0f, 0.5f, -2.0f),
                                       .yaw = yaw,
                                       .pitch = pitch,
                                       .crouching = crouching,
                                       .aiming = aiming,
                                       .weapon = hold};
            for (int frame = 0; frame < 60; ++frame) {
              animator.Update(input);
            }
            REQUIRE(animator.weapon());
            INFO(std::string(file), " yaw ", yaw, " aiming ", aiming,
                 " crouching ", crouching, " pitch ", degrees);
            const float expected_pitch =
                aiming ? pitch
                       : std::max(pitch - kLowReadyDip, kLowReadyMinPitch);
            const glm::vec3 muzzle =
                glm::normalize(glm::vec3(animator.root() * *animator.weapon() *
                                         glm::vec4(hold.forward, 0.0f)));
            CHECK(glm::dot(muzzle, LookDirection(yaw, expected_pitch)) >
                  std::cos(glm::radians(2.0f)));
            // Character space: the stock before the right shoulder joint;
            // none of stock, grip, sight and muzzle at head height (above
            // the head joint, the top of the neck) behind the face.
            const glm::vec3 stock = Apply(*animator.weapon(), hold.stock);
            CHECK(stock.z < animator.joint_position(Joint::kRightUpperArm).z);
            const glm::vec3 head = animator.joint_position(Joint::kHead);
            for (const glm::vec3 part :
                 {hold.stock, hold.grip, hold.sight, muzzle_point}) {
              const glm::vec3 point = Apply(*animator.weapon(), part);
              if (point.y > head.y) {
                CHECK(point.z < head.z + face);
              }
            }
          }
        }
      }
    }
  }
}

TEST_CASE("Walking forwards, toes and muzzle point the way the player goes") {
  const WeaponHold hold = Hold(HeldWeapon::kRifle, "m16.pbo");
  for (const float yaw : {0.0f, 0.8f, 3.0f, -1.7f}) {
    CharacterAnimator animator(Rig());
    // Forward at yaw 0 is -Z, as camera::FlatForwardFrom().
    const glm::vec3 forward = LookDirection(yaw, 0.0f);
    const CharacterInput input{
        .dt = kDt, .velocity = forward * 2.0f, .yaw = yaw, .weapon = hold};
    for (int frame = 0; frame < 90; ++frame) {
      animator.Update(input);
    }
    INFO("yaw ", yaw);
    for (const auto& [ankle, toe] :
         {std::pair{Joint::kLeftAnkle, Joint::kLeftToe},
          std::pair{Joint::kRightAnkle, Joint::kRightToe}}) {
      glm::vec3 along = glm::vec3(animator.root() *
                                  glm::vec4(animator.joint_position(toe) -
                                                animator.joint_position(ankle),
                                            0.0f));
      along.y = 0.0f;
      CHECK(glm::dot(glm::normalize(along), forward) > 0.9f);
    }
    REQUIRE(animator.weapon());
    glm::vec3 muzzle = glm::vec3(animator.root() * *animator.weapon() *
                                 glm::vec4(hold.forward, 0.0f));
    muzzle.y = 0.0f;
    CHECK(glm::dot(glm::normalize(muzzle), forward) > 0.9f);
  }
}

TEST_CASE("Remote players are animated once there is a rig") {
  const WeaponHold rifle = Hold(HeldWeapon::kRifle, "m16.pbo");
  RemotePlayers players;
  const RemotePlayerView view{.id = 3,
                              .feet = glm::vec3(4.0f, 0.0f, -2.0f),
                              .yaw = 0.5f,
                              .weapon = weapon::WeaponDefId{},
                              .velocity = glm::vec3(0.0f, 0.0f, -2.0f),
                              .hold = rifle};
  players.Update(std::span(&view, 1), kDt);
  REQUIRE(players.placements().size() == 1);
  CHECK(players.placements()[0].animator == nullptr);  // capsules

  players.SetRig(Rig());
  CHECK(players.animated());
  for (int frame = 0; frame < 30; ++frame) {
    players.Update(std::span(&view, 1), kDt);
  }
  REQUIRE(players.placements().size() == 1);
  const RemotePlayerPlacement& placement = players.placements()[0];
  REQUIRE(placement.animator != nullptr);
  REQUIRE(placement.animator->weapon());
  CHECK(glm::length(glm::vec3(placement.animator->root()[3]) - view.feet) <
        1.0e-5f);
  // The weapon is in the model's hands; the tag floats over its head.
  const glm::mat4 in_hands =
      placement.animator->root() * *placement.animator->weapon();
  CHECK(glm::length(glm::vec3(placement.weapon_root[3] - in_hands[3])) <
        1.0e-5f);
  CHECK(placement.name_anchor.y > 1.8f);
  CHECK(placement.name_anchor.y < 2.2f);

  // Dead: not placed; back: placed again.
  RemotePlayerView dead = view;
  dead.alive = false;
  players.Update(std::span(&dead, 1), kDt);
  CHECK(players.placements().empty());
  players.Update(std::span(&view, 1), kDt);
  CHECK(players.placements().size() == 1);
}

// The chest's forward lean from the vertical (degrees), ribs joint to neck.
float ChestLean(const CharacterAnimator& animator) {
  const glm::vec3 chest = animator.joint_position(Joint::kNeck) -
                          animator.joint_position(Joint::kRibs);
  return glm::degrees(std::atan2(-chest.z, chest.y));
}

TEST_CASE("The upper body leans into walking and sprinting") {
  const WeaponHold hold = Hold(HeldWeapon::kRifle, "m16.pbo");
  CharacterAnimator idle(Rig());
  for (int frame = 0; frame < 60; ++frame) {
    idle.Update({.dt = kDt, .weapon = hold});
  }
  const auto lean_at = [&](float speed) {
    CharacterAnimator animator(Rig());
    for (int frame = 0; frame < 120; ++frame) {
      animator.Update({.dt = kDt,
                       .velocity = glm::vec3(0.0f, 0.0f, -speed),
                       .weapon = hold});
    }
    return ChestLean(animator);
  };
  const float walk = lean_at(3.5f);
  const float sprint = lean_at(7.0f);
  MESSAGE("chest lean from vertical: idle ", ChestLean(idle), " deg, 3.5 m/s ",
          walk, " deg, 7 m/s ", sprint, " deg");
  CHECK(walk > 5.0f);
  CHECK(walk < 9.0f);
  CHECK(sprint > 10.0f);
  CHECK(sprint < 13.0f);
}

TEST_CASE("Hostile input never produces non-finite transforms") {
  constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
  constexpr float kInf = std::numeric_limits<float>::infinity();
  WeaponHold broken = Hold(HeldWeapon::kRifle, "m16.pbo");
  broken.grip = glm::vec3(kNan);
  const CharacterInput inputs[] = {
      {.dt = kNan},
      {.dt = kInf, .velocity = glm::vec3(kInf, 0.0f, 0.0f)},
      {.dt = -1.0f, .feet = glm::vec3(kNan), .yaw = kInf},
      {.dt = 1.0e9f, .velocity = glm::vec3(1.0e30f), .pitch = kNan},
      {.dt = kDt, .yaw = 1.0e30f, .pitch = 1.0e30f, .weapon = broken},
      {.dt = kDt, .velocity = glm::vec3(-kInf), .on_ground = false},
      {.dt = kDt, .alive = false},
  };
  CharacterAnimator animator(Rig());
  for (int round = 0; round < 3; ++round) {
    for (const CharacterInput& input : inputs) {
      animator.Update(input);
      CHECK(AllFinite(animator));
      CharacterInput sane{.dt = kDt,
                          .velocity = glm::vec3(3.0f, 0.0f, 0.0f),
                          .weapon = Hold(HeldWeapon::kPistol, "glock17.pbo")};
      animator.Update(sane);
      CHECK(AllFinite(animator));
    }
  }
}

TEST_CASE("State changes blend without jumps") {
  const WeaponHold rifle = Hold(HeldWeapon::kRifle, "m16.pbo");
  const WeaponHold pistol = Hold(HeldWeapon::kPistol, "glock17.pbo");
  CharacterAnimator animator(Rig());
  CharacterInput input{.dt = kDt, .weapon = rifle};
  animator.Update(input);
  std::array<glm::vec3, kJointCount> previous{};
  const auto snapshot = [&] {
    for (int j = 0; j < kJointCount; ++j) {
      previous[static_cast<size_t>(j)] =
          animator.joint_position(static_cast<Joint>(j));
    }
  };
  snapshot();
  float worst = 0.0f;
  for (int frame = 1; frame < 1200; ++frame) {
    const int step = frame / 60;  // a new state every second
    input.crouching = step % 3 == 1;
    input.aiming = step % 2 == 1;
    input.on_ground = step % 7 != 5;
    input.weapon = step % 4 == 2   ? pistol
                   : step % 4 == 3 ? WeaponHold{}
                                   : rifle;
    input.velocity =
        step % 5 < 2 ? glm::vec3(0.0f, 0.0f, -2.0f) : glm::vec3(0.0f);
    // Jittery remote state: the pitch wobbles a little every frame.
    input.pitch = 0.3f * std::sin(0.05f * static_cast<float>(frame)) +
                  0.01f * static_cast<float>(frame % 3);
    animator.Update(input);
    for (int j = 0; j < kJointCount; ++j) {
      const glm::vec3 now = animator.joint_position(static_cast<Joint>(j));
      const float jump = glm::length(now - previous[static_cast<size_t>(j)]);
      worst = std::max(worst, jump);
    }
    snapshot();
  }
  // Fast but blended: a crouch drops the hips 0.5 m over 0.18 s and the
  // hands travel up to 0.7 m between weapon and sides in 0.15 s (at most
  // about 0.15 m a frame); a pop moves it all in one frame, as a snap shows.
  CHECK(worst < 0.2f);
  input = CharacterInput{.dt = kDt, .weapon = rifle};
  for (int frame = 0; frame < 30; ++frame) {
    animator.Update(input);
  }
  snapshot();
  input.crouching = true;
  input.weapon = WeaponHold{};
  animator.Reset();
  animator.Update(input);
  float popped = 0.0f;
  for (int j = 0; j < kJointCount; ++j) {
    popped = std::max(
        popped, glm::length(animator.joint_position(static_cast<Joint>(j)) -
                            previous[static_cast<size_t>(j)]));
  }
  CHECK(popped > 0.3f);
}

}  // namespace
}  // namespace blue::game
