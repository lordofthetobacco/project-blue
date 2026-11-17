#ifndef BLUE_WEAPON_WEAPON_ANIMATOR_H_
#define BLUE_WEAPON_WEAPON_ANIMATOR_H_

#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/clip.h"
#include "animation/clip_player.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"

namespace blue::weapon {

// Where the shot and the slide sit in a weapon's authored fire clip.
// Defaults: samples/glock/glock17.glb, whose merged clip (1.5 s) pulls the
// trigger from 0.2 s; the slide leaves rest after the key at 0.333 s and is
// rearmost at 0.4 s (FiringKeyTimes() on that file reports both).
struct WeaponAnimatorTuning {
  float fire_time = 0.333f;  // s into the clip where the shot breaks
  // Node whose channels hold the slide (or bolt) travel. Only read during
  // construction.
  std::string_view slide_node = "Slide";
  float lock_time = 0.4f;  // s into the clip where the slide is rearmost
};

// Key times of a fire clip, measured from its channels.
struct FiringKeyTimes {
  // Last key before the node's first change from its first value; -1 when
  // the node never moves.
  float motion_start = -1.0f;
  // First key of the largest translation along the motion's first moving
  // axis (the rearmost slide position); -1 when the node never moves.
  float extreme = -1.0f;
};

// Returns where `node`'s translation channels in `clip` start moving and
// reach their extreme. Every channel must satisfy animation::IsValid().
[[nodiscard]] FiringKeyTimes MeasureFiringKeys(const animation::Clip& clip,
                                               int node);

// Drives a weapon model's authored fire animation from the firearm's events.
//
// Each frame with shots restarts the clip at `fire_time` (several shots in a
// frame restart it once); it then plays at 1x to its end and the model rests
// in the rest pose. While the bolt is locked back (empty), the slide node's
// channels are held at `lock_time` (after travelling there if a shot just
// started the clip). The muzzle used for bullets stays the viewmodel's: the
// animation is visual only.
//
// Borrows the hierarchy and the clip: both must outlive the animator and
// stay unchanged; the clip's channels must index the hierarchy and satisfy
// animation::IsValid(). Allocates only in the constructor. Copyable (copies
// share the borrowed data). Not thread-safe.
class WeaponAnimator {
 public:
  // `hierarchy` and `fire_clip` must not be null.
  WeaponAnimator(const animation::Hierarchy* hierarchy,
                 const animation::Clip* fire_clip,
                 const WeaponAnimatorTuning& tuning = {});
  WeaponAnimator(const WeaponAnimator&) = default;
  WeaponAnimator& operator=(const WeaponAnimator&) = default;

  // Advances by `dt` seconds (negative counts as 0) with this frame's
  // FirearmEvents::shots and Firearm::bolt_locked_back(), and recomputes
  // pose().
  void Update(int shots, bool bolt_locked_back, float dt);

  // Stops the clip and returns to the rest pose (slide forward), e.g. when
  // the weapon is drawn again. Call Update() to apply a locked bolt.
  void Reset();

  // Local transform of every hierarchy node, as last computed.
  [[nodiscard]] std::span<const animation::Transform> pose() const {
    return pose_;
  }

  // Writes world[i] = root * (node i's model-space transform in pose()) for
  // every node; pass Viewmodel::model_matrix() as `root`. `world` must hold
  // at least one matrix per node (extra entries are left alone). Does not
  // allocate.
  void ComputeWorld(const glm::mat4& root, std::span<glm::mat4> world) const;

  // True while the fire clip plays; time() is its position in seconds.
  [[nodiscard]] bool playing() const { return player_.playing(); }
  [[nodiscard]] float time() const { return player_.time(); }
  [[nodiscard]] int slide_node() const { return slide_node_; }
  [[nodiscard]] const WeaponAnimatorTuning& tuning() const { return tuning_; }

 private:
  const animation::Hierarchy* hierarchy_;
  const animation::Clip* fire_clip_;
  WeaponAnimatorTuning tuning_;
  int slide_node_ = -1;  // -1 when the hierarchy has no such node
  // The fire clip's channels that target the slide node.
  animation::Clip slide_clip_;
  animation::ClipPlayer player_;
  std::vector<animation::Transform> pose_;  // one per hierarchy node
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_ANIMATOR_H_
