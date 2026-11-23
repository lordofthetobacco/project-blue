#ifndef BLUE_WEAPON_WEAPON_THROW_H_
#define BLUE_WEAPON_WEAPON_THROW_H_

#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"

namespace blue::weapon {

// How a weapon leaves the hands.
struct ThrowTuning {
  float speed = 3.5f;  // m/s along the view, added to the player's velocity
  float lift = 1.0f;   // m/s upwards on top of that
  float spin = 3.0f;   // rad/s about the world's up axis
  // m kept between the weapon and whatever blocks its way out of the hands.
  float clearance = 0.05f;
};

// Returns the gentler toss of a weapon swapped out by a pick-up: it lands
// just in front of the player.
[[nodiscard]] ThrowTuning SwapTossTuning();

// The weapon in hand at the moment it is thrown.
struct ThrowInput {
  // Prefab space -> world: where the model is drawn in the hands
  // (Viewmodel::model_matrix()). Its columns may carry a scale.
  glm::mat4 held{1.0f};
  asset::Bounds bounds;  // prefab space, the collision box
  glm::vec3 eye{0.0f};
  glm::vec3 forward{0.0f, 0.0f, -1.0f};  // unit view direction
  glm::vec3 player_velocity{0.0f};       // m/s
};

// The segment from the eye to the far side of the held weapon's box along
// the view, which must be free for the weapon to leave from the hands (a
// player hugging a wall holds the weapon partly inside it).
struct ThrowProbe {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};  // unit
  float length = 0.0f;                     // m
};

// Returns the probe for `input`.
[[nodiscard]] ThrowProbe MakeThrowProbe(const ThrowInput& input);

// Where and how the thrown weapon's body starts.
struct ThrowPlan {
  glm::vec3 position{0.0f};  // of the prefab origin
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};
  glm::vec3 linear_velocity{0.0f};   // m/s
  glm::vec3 angular_velocity{0.0f};  // rad/s
  bool blocked = false;              // the fallback below was taken
};

// Plans the throw of `input` given what the probe (MakeThrowProbe()) hit:
// `blocked_at` is the distance along it to the first obstacle, nullopt for
// none. Free: the weapon starts where it is held with the player's velocity
// plus the tuning's speed, lift and spin. Blocked: it is pulled back along
// the probe to keep `clearance` from the obstacle (never further than to
// put the box centre on the eye) and simply dropped with the player's
// velocity, no spin.
[[nodiscard]] ThrowPlan PlanThrow(const ThrowInput& input,
                                  const ThrowTuning& tuning,
                                  std::optional<float> blocked_at);

// Returns the prefab space -> world matrix of `plan`'s pose.
[[nodiscard]] glm::mat4 PlanMatrix(const ThrowPlan& plan);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_THROW_H_
