#ifndef BLUE_GAME_REMOTE_PLAYERS_H_
#define BLUE_GAME_REMOTE_PLAYERS_H_

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "game/character_animator.h"
#include "physics/character.h"
#include "player/player_controller.h"
#include "weapon/weapon_def.h"

// Where the other players of a networked game are drawn. With a player
// model rig (SetRig()) each live player is animated by its own
// CharacterAnimator (the game draws the model posed by it, the weapon in its
// hands); without one, each is a capsule of the character's size
// (crouch-aware) holding the prefab of its weapon at a fixed hand offset.
// The game feeds the interpolated poses every frame and draws what
// placements() lists (modelled on weapon::DroppedWeapons).

namespace blue::game {

// The held weapon sits this far from the eye in the player's view frame
// (x right, y up, -z forward), metres.
inline constexpr glm::vec3 kRemoteHandOffset{0.22f, -0.28f, -0.35f};
// Name tags float this far above the top of the capsule, metres.
inline constexpr float kNameTagLift = 0.25f;

// One remote player as the game knows it this frame.
struct RemotePlayerView {
  uint8_t id = 0;
  glm::vec3 feet{0.0f};
  float yaw = 0.0f;  // camera::LookAngles convention
  float pitch = 0.0f;
  bool crouching = false;
  bool alive = true;
  // The definition of the weapon in hand (the game's library), if any.
  std::optional<weapon::WeaponDefId> weapon;
  // For the animated model: world velocity (m/s), standing on the ground,
  // aiming, and how the weapon in hand is held (kNone when unarmed).
  glm::vec3 velocity{0.0f};
  bool on_ground = true;
  bool aiming = false;
  WeaponHold hold;
};

// Where to draw one remote player.
struct RemotePlayerPlacement {
  uint8_t id = 0;
  bool crouching = false;  // draw the crouched capsule
  // Capsule mesh space (feet at the origin, asset::MakeCapsule) -> world,
  // turned by the yaw.
  glm::mat4 body{1.0f};
  std::optional<weapon::WeaponDefId> weapon;
  // The weapon prefab's space -> world: in the model's hands when
  // animated, else at the fixed hand offset along the look (forward +z).
  glm::mat4 weapon_root{1.0f};
  // World point above the head for the name tag.
  glm::vec3 name_anchor{0.0f};
  // The player's animator when there is a rig, else null (draw the
  // capsule). Valid until the next Update(), SetRig() or Clear().
  const CharacterAnimator* animator = nullptr;
};

// Turns remote player views into placements. Reserves room for every
// player id once; no allocation per frame. Copyable.
class RemotePlayers {
 public:
  explicit RemotePlayers(const physics::CharacterDesc& character = {},
                         const player::PlayerTuning& tuning = {});
  RemotePlayers(const RemotePlayers&) = default;
  RemotePlayers& operator=(const RemotePlayers&) = default;

  // Animates remote players with `rig` from now on (nullopt: capsules).
  // Players start over from their next Update().
  void SetRig(std::optional<CharacterRig> rig);
  [[nodiscard]] bool animated() const { return rig_.has_value(); }

  // Replaces the placements with those of `players`, animated over `dt`
  // seconds; dead players are left out (their animation starts over when
  // they are back). Allocates only on a player's first animated frame.
  void Update(std::span<const RemotePlayerView> players, float dt = 0.0f);
  void Clear();

  [[nodiscard]] std::span<const RemotePlayerPlacement> placements() const {
    return placements_;
  }
  // The animator of player `id` with its last animated pose (kept while the
  // player is dead and not updated; Reset() keeps the pose), or null when the
  // id is out of range or the player was never animated. Valid until the next
  // Update(), SetRig() or Clear().
  [[nodiscard]] const CharacterAnimator* animator(uint8_t id) const {
    if (id >= animators_.size() || !animators_[id]) {
      return nullptr;
    }
    return &*animators_[id];
  }
  // Capsule size the meshes must be made with (m).
  [[nodiscard]] float radius() const { return character_.radius; }
  [[nodiscard]] float height(bool crouching) const {
    return crouching ? character_.crouch_height : character_.standing_height;
  }
  // Eye height above the feet (m).
  [[nodiscard]] float eye_height(bool crouching) const {
    return crouching ? tuning_.eye_height_crouching
                     : tuning_.eye_height_standing;
  }

 private:
  physics::CharacterDesc character_;
  player::PlayerTuning tuning_;
  std::vector<RemotePlayerPlacement> placements_;
  std::optional<CharacterRig> rig_;
  // Per player id; made on a player's first animated frame.
  std::vector<std::optional<CharacterAnimator>> animators_;
  std::vector<bool> seen_;  // per player id: placed this frame
};

}  // namespace blue::game

#endif  // BLUE_GAME_REMOTE_PLAYERS_H_
