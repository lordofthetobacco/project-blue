#ifndef BLUE_INTERACT_INTERACTION_FOCUS_H_
#define BLUE_INTERACT_INTERACTION_FOCUS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

namespace blue::interact {

// What an interactable is; the game decides per kind what interacting does.
// Doors, NPCs and loot are later additions.
enum class InteractableKind : uint8_t {
  kDroppedWeapon,  // A weapon the player dropped: picked up.
  kSceneWeapon,    // A weapon placed in the scene: picked up.
};

// A box in world space: `axes` holds its unit local x, y and z axes (columns,
// orthonormal), `half_extents` its half sizes along them (negative values
// count as their magnitude).
struct OrientedBox {
  glm::vec3 center{0.0f};
  glm::mat3 axes{1.0f};
  glm::vec3 half_extents{0.0f};
};

// One thing the player could interact with this frame, as handed in by the
// game.
struct Interactable {
  // Chosen by the game; unique among one frame's candidates of its kind.
  uint32_t id = 0;
  InteractableKind kind = InteractableKind::kDroppedWeapon;
  OrientedBox shape;  // What the player aims at.
};

// What has the focus.
struct Focus {
  uint32_t id = 0;
  InteractableKind kind = InteractableKind::kDroppedWeapon;

  friend bool operator==(const Focus&, const Focus&) = default;
};

// How close and how precisely the player must look at a shape. A shape is
// focusable when its closest point to the eye is within `reach` and the view
// ray passes it by no more than `aim_radius` and no more than
// `max_aim_angle` as seen from the eye: far away the radius applies (12 cm
// at 2.5 m is 2.7 degrees), close by the angle does (at 0.5 m 7.5 cm), so a
// small item does not need to be hit exactly and a nearby one does not catch
// a glance going past it.
//
// The focused shape keeps the focus until it is clearly out, with every
// limit widened by the release values, or a neighbour is clearly better:
// the ray passes the neighbour by `switch_angle` less, or, both being looked
// at straight on, it is `switch_distance` nearer.
struct FocusTuning {
  float reach = 2.5f;           // m
  float aim_radius = 0.12f;     // m
  float max_aim_angle = 0.15f;  // rad, about 8.6 degrees
  float release_reach = 0.25f;  // m added to `reach` for the focused shape
  // Factor on `aim_radius` and `max_aim_angle` for the focused shape.
  float release_aim_scale = 1.5f;
  float switch_angle = 0.02f;    // rad
  float switch_distance = 0.1f;  // m
};

// Where the player looks from: `direction` need not be normalised; zero or
// non-finite input focuses nothing.
struct FocusQuery {
  glm::vec3 eye{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// How the view ray passes one shape.
struct AimMeasure {
  float reach_distance = 0.0f;  // m, eye to the shape's closest point
  float miss_distance = 0.0f;   // m, ray to the shape; 0 when the ray hits it
  // m along the ray to where it passes the shape closest (where it enters
  // the shape when it hits; 0 with the eye inside).
  float along = 0.0f;
  float miss_angle = 0.0f;  // rad, atan(miss_distance / along)
  // The point of the shape the ray passes closest (where it enters on a
  // hit): what a line of sight test should aim at.
  glm::vec3 aim_point{0.0f};
};

// Returns how the ray from `eye` along the unit `direction` passes `box`.
[[nodiscard]] AimMeasure MeasureAim(glm::vec3 eye, glm::vec3 direction,
                                    const OrientedBox& box);

// A focusable candidate: its index into the candidates and its measure.
struct RankedCandidate {
  size_t index = 0;
  AimMeasure aim;
};

// Picks the interactable the player is looking at, frame after frame, with
// hysteresis (FocusTuning). Line of sight is not its business: the caller
// rejects hidden candidates through the predicate of Update(), or Rank()s
// them and makes the choice itself (SetFocus()).
//
// No allocation once the candidate count has been seen. Copyable. Not
// thread-safe.
class InteractionFocus {
 public:
  explicit InteractionFocus(const FocusTuning& tuning = {}) : tuning_(tuning) {}

  // Returns the focusable `candidates` in preference order: the ray passing
  // closest (by angle) first, ties by distance, and the current focus ahead
  // of any neighbour that is not clearly better. Valid until the next call.
  [[nodiscard]] std::span<const RankedCandidate> Rank(
      const FocusQuery& query, std::span<const Interactable> candidates);

  // Ranks `candidates` and focuses the first one `is_visible(candidate,
  // aim_point)` accepts, or nothing. Returns the new focus.
  template <typename IsVisible>
  std::optional<Focus> Update(const FocusQuery& query,
                              std::span<const Interactable> candidates,
                              IsVisible&& is_visible) {
    std::optional<Focus> chosen;
    for (const RankedCandidate& ranked : Rank(query, candidates)) {
      const Interactable& candidate = candidates[ranked.index];
      if (is_visible(candidate, ranked.aim.aim_point)) {
        chosen = Focus{.id = candidate.id, .kind = candidate.kind};
        break;
      }
    }
    SetFocus(chosen);
    return chosen;
  }

  void SetFocus(std::optional<Focus> focus) { focus_ = focus; }
  [[nodiscard]] const std::optional<Focus>& focus() const { return focus_; }
  [[nodiscard]] const FocusTuning& tuning() const { return tuning_; }

 private:
  FocusTuning tuning_;
  std::optional<Focus> focus_;
  std::vector<RankedCandidate> ranked_;  // reused by Rank()
};

}  // namespace blue::interact

#endif  // BLUE_INTERACT_INTERACTION_FOCUS_H_
