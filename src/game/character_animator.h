#ifndef BLUE_GAME_CHARACTER_ANIMATOR_H_
#define BLUE_GAME_CHARACTER_ANIMATOR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/result.h"
#include "prefab/prefab.h"

// Procedural animation of the player model ("Robot Kyle", kyle.pbo): a
// rigid-part figure whose nodes are its bones. No clips: the gait follows
// speed and distance travelled, legs and arms are placed by two-bone IK, the
// hands hold the weapon. Cosmetic only; nothing here feeds back into the
// simulation.
//
// Spaces: "figure space" is the model's prefab space (metres, +Y up, the
// figure faces -Z, its right is +X, origin = between the feet on the
// ground); "character space" is figure space placed at the character's feet
// and turned by its yaw (CharacterRoot()).

namespace blue::game {

// The model's joints the animator drives, one node each.
enum class Joint : int {
  kRoot,  // the pelvis; carries Hip and Ribs
  kHip,
  kRibs,
  kNeck,
  kHead,
  kLeftShoulder,
  kLeftUpperArm,
  kLeftForearm,
  kLeftWrist,
  kLeftThigh,
  kLeftKnee,
  kLeftAnkle,
  kLeftToe,
  kRightShoulder,
  kRightUpperArm,
  kRightForearm,
  kRightWrist,
  kRightThigh,
  kRightKnee,
  kRightAnkle,
  kRightToe,
};
inline constexpr int kJointCount = 21;

enum class Side : int { kLeft = 0, kRight = 1 };

// Returns the node name of `joint` ("Root", "Left_Upper_Arm", ...).
[[nodiscard]] std::string_view JointName(Joint joint);

// The rest geometry of a player model, read from its hierarchy: every
// joint's node, its rest offset from its parent joint (the bone vectors the
// IK needs: upper arm = offset of the forearm, ...) and position, and the
// palm frames. Requirements (MakeCharacterRig() checks them): every joint
// node exists, joints are parented as the bones are, joint nodes have
// identity rest rotation and unit scale (their axes are the figure axes),
// the root joint has no parent.
struct CharacterRig {
  size_t node_count = 0;
  std::array<int, kJointCount> nodes{};
  std::array<glm::vec3, kJointCount> rest_offset{};    // from the parent
  std::array<glm::vec3, kJointCount> rest_position{};  // figure space
  // Per Side: the palm centre and hand frame (+Z along the flat hand's
  // fingers, +Y the palm normal) relative to the wrist joint, from the
  // sockets hand_l / hand_r.
  std::array<animation::Transform, 2> palm{};
  // Top of the head relative to the head joint (socket "head"; the head
  // joint itself when the socket is missing).
  glm::vec3 head_top{0.0f};
  // Every node's rest local transform (non-joint nodes keep it).
  std::vector<animation::Transform> rest;
};

// Builds the rig of a model with hierarchy `hierarchy` and `sockets`
// (hand_l and hand_r required, head optional). Fails with a message naming
// the first broken requirement.
[[nodiscard]] core::Result<CharacterRig, std::string> MakeCharacterRig(
    const animation::Hierarchy& hierarchy,
    std::span<const prefab::Socket> sockets);

enum class HeldWeapon : uint8_t { kNone, kPistol, kRifle };

// How a held weapon is gripped, in its prefab space (metres).
struct WeaponHold {
  HeldWeapon kind = HeldWeapon::kNone;
  glm::vec3 forward{0.0f, 0.0f, 1.0f};  // bore direction, unit
  glm::vec3 up{0.0f, 1.0f, 0.0f};       // unit, perpendicular to forward
  glm::vec3 grip{0.0f};                 // the firing hand's palm centre
  glm::vec3 support{0.0f};              // the support hand's palm centre
  glm::vec3 sight{0.0f};                // the sight's eye point
  glm::vec3 stock{0.0f};                // rifle: the butt, against the shoulder
};

// Returns the hold of a weapon of `kind` whose prefab is `prefab`: forward
// and the eye point from socket "sight", the grip from socket "grip"
// (defaults from the prefab bounds where one is missing), the support grip
// and the stock at sensible defaults per kind. kNone gives the default hold.
[[nodiscard]] WeaponHold MakeWeaponHold(HeldWeapon kind,
                                        const prefab::Prefab& prefab);

// At low ready (not aiming) the weapon points along the look dipped by
// kLowReadyDip, but never below kLowReadyMinPitch (radians); aimed, it
// points along the look.
inline constexpr float kLowReadyDip = 0.5f;
inline constexpr float kLowReadyMinPitch = -1.1f;

// What the animator is told about the character each frame.
struct CharacterInput {
  float dt = 0.0f;  // s since the previous Update()
  glm::vec3 feet{0.0f};
  glm::vec3 velocity{0.0f};  // world, m/s
  float yaw = 0.0f;          // camera::LookAngles convention
  float pitch = 0.0f;        // radians, positive looks up
  bool crouching = false;
  bool on_ground = true;
  bool aiming = false;
  bool alive = true;
  WeaponHold weapon;
};

// The result of SolveTwoBoneIk().
struct TwoBoneIk {
  glm::vec3 middle{0.0f};  // the elbow / knee
  glm::vec3 end{0.0f};     // the wrist / ankle: `target` when reachable
};

// Places a two-bone chain from `root` with bone lengths `upper` and `lower`
// towards `target`, bending the middle joint towards `pole` (a direction).
// A target out of reach gets the limb fully extended pointing at it (too
// close: folded as far as the lengths allow). Never returns NaN for finite
// input; zero lengths give a degenerate but finite chain.
[[nodiscard]] TwoBoneIk SolveTwoBoneIk(glm::vec3 root, glm::vec3 target,
                                       float upper, float lower,
                                       glm::vec3 pole);

// Returns the character space -> world matrix: at `feet`, turned by `yaw`
// about +Y.
[[nodiscard]] glm::mat4 CharacterRoot(glm::vec3 feet, float yaw);

// Animates one character. Copies the rig; copyable. Allocates only at
// construction. Not thread-safe.
class CharacterAnimator {
 public:
  explicit CharacterAnimator(CharacterRig rig);

  // Advances by input.dt and poses the model for `input`. Hostile input
  // (NaN, infinities, huge or negative dt) is clamped; outputs stay finite.
  // The first Update() and the first after Reset() or a dead frame snap
  // every blend to its target.
  void Update(const CharacterInput& input);
  // The next Update() snaps (e.g. after a teleport or respawn).
  void Reset() { snap_ = true; }

  // One local transform per node of the model's hierarchy.
  [[nodiscard]] std::span<const animation::Transform> locals() const {
    return locals_;
  }
  // The held weapon's prefab space -> character space; nullopt unarmed.
  [[nodiscard]] const std::optional<glm::mat4>& weapon() const {
    return weapon_;
  }
  // Character space -> world for the last input.
  [[nodiscard]] const glm::mat4& root() const { return root_; }

  // Joint and palm positions, character space, of the last Update().
  [[nodiscard]] glm::vec3 joint_position(Joint joint) const {
    return position_[static_cast<size_t>(joint)];
  }
  [[nodiscard]] glm::vec3 palm_position(Side side) const;
  // Top of the head, character space.
  [[nodiscard]] glm::vec3 head_top() const;
  // Where the ankle of `side` was asked to go (character space).
  [[nodiscard]] glm::vec3 foot_target(Side side) const {
    return foot_target_[static_cast<size_t>(side)];
  }
  // True while the foot of `side` is in the stance part of the stride and
  // the gait is fully blended in (walking on the ground).
  [[nodiscard]] bool foot_planted(Side side) const {
    return planted_[static_cast<size_t>(side)];
  }
  // The gait cycle, [0, 1): 0 = the left foot sets down.
  [[nodiscard]] float gait_phase() const { return phase_; }
  [[nodiscard]] const CharacterRig& rig() const { return rig_; }

 private:
  struct Frame {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  };

  void Blend(const CharacterInput& input, float dt);
  // The length of a gait cycle (m) at the current speed and direction.
  float Cycle() const;
  // The forward lean of the upper body (rad): crouch and walking forwards.
  float Lean() const;
  // The upper body's share of the look pitch (rad, clamped).
  float SpinePitch() const;
  // The upper body's turn about +Y for the bladed rifle stance (rad).
  float TorsoTwist() const;
  void PoseLegs();
  void PoseSpine();
  void PoseWeapon(const CharacterInput& input, float dt);
  void PoseArms();
  void PlaceChild(Joint joint);
  void PoseLimb(Joint upper, Joint lower, Joint end, glm::vec3 target,
                glm::vec3 pole, glm::vec3 fold);
  // Writes locals_ from the joint frames; false (rest pose written, state
  // reset) if any came out non-finite.
  bool WriteLocals();

  CharacterRig rig_;
  // How far the rest pose's chest leans back from the vertical (rad).
  float rest_lean_ = 0.0f;
  std::vector<animation::Transform> locals_;
  std::array<glm::vec3, kJointCount> position_{};
  std::array<glm::quat, kJointCount> rotation_{};
  std::array<glm::vec3, 2> foot_target_{};
  std::array<bool, 2> planted_{};
  std::optional<glm::mat4> weapon_;
  glm::mat4 root_{1.0f};

  // Blended state.
  bool snap_ = true;
  float pitch_ = 0.0f;
  float phase_ = 0.0f;                // gait cycle, [0, 1)
  float speed_ = 0.0f;                // smoothed horizontal speed, m/s
  glm::vec2 direction_{0.0f, -1.0f};  // smoothed move direction, x/z
  // State weights in [0, 1], eased from linear ramps (the *_ramp_).
  float crouch_ = 0.0f;
  float air_ = 0.0f;
  float aim_ = 0.0f;
  float armed_ = 0.0f;
  float walk_ = 0.0f;  // the gait blended in (moving, on the ground)
  float crouch_ramp_ = 0.0f;
  float air_ramp_ = 0.0f;
  float aim_ramp_ = 0.0f;
  float armed_ramp_ = 0.0f;
  float walk_ramp_ = 0.0f;
  float landing_ = 0.0f;  // 1 at touch-down, falls to 0
  float breath_ = 0.0f;   // s
  HeldWeapon kind_ = HeldWeapon::kNone;
  WeaponHold hold_;
  Frame grip_;  // the firing grip: weapon grip point and weapon rotation
  std::array<Frame, 2> palm_target_{};  // per Side
};

}  // namespace blue::game

#endif  // BLUE_GAME_CHARACTER_ANIMATOR_H_
