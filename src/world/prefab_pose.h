#ifndef BLUE_WORLD_PREFAB_POSE_H_
#define BLUE_WORLD_PREFAB_POSE_H_

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/clip_player.h"
#include "animation/transform.h"
#include "render/scene_view.h"
#include "world/prefab_cache.h"

namespace blue::world {

// One posed copy of a cached prefab: its node pose, node matrices in prefab
// space and an optional playing clip. The game's weapons, the editor's prefab
// preview and every placed prefab of a World use it. Borrows `cache`, which
// must outlive it; copyable (the copy borrows the same cache). Not
// thread-safe.
//
// Static use costs nothing per frame: matrices are computed at construction
// and only recomputed by Update() while a clip plays, or by SetTime() /
// ResetToRest(). Nothing allocates after construction.
class PrefabPose {
 public:
  // The rest pose of `prefab`.
  PrefabPose(const PrefabCache& cache, PrefabId prefab);
  PrefabPose(const PrefabPose&) = default;
  PrefabPose& operator=(const PrefabPose&) = default;

  // Starts clip `clip` (index into the prefab's clips) at `start_time` with
  // `speed`; `loop` defaults to the clip's own loop flag. Returns false
  // (nothing changes) if the index is out of range.
  bool Play(int clip, float start_time = 0.0f, float speed = 1.0f,
            std::optional<bool> loop = std::nullopt);
  // Like Play() by clip name.
  bool Play(std::string_view clip_name, float start_time = 0.0f,
            float speed = 1.0f, std::optional<bool> loop = std::nullopt);
  // Stops advancing and keeps the current pose.
  void Stop() { player_.Stop(); }
  // Back to the rest pose with no clip.
  void ResetToRest();
  // Poses the current clip at `time` without playing it (editor scrubbing).
  // No-op without a clip.
  void SetTime(float time);
  // Poses the nodes from `local` (one local transform per node, e.g. a
  // procedural animator's output) and recomputes the matrices; a playing
  // clip is stopped and forgotten. Copies min(nodes, local.size())
  // entries, the others keep their rest transform. Does not allocate.
  void SetLocalPose(std::span<const animation::Transform> local);
  // Advances a playing clip by `dt` seconds and recomputes the matrices.
  // Returns true if the pose changed.
  bool Update(float dt);

  // Returns the clip events crossed by the last Update(), i.e. whose time is
  // in (previous time, current time] in play direction (wrapping across a
  // loop); the first Update() after Play() includes the start time, so an
  // event at 0 fires. Valid until the next Update(); empty when nothing was
  // crossed.
  [[nodiscard]] std::span<const animation::ClipEvent* const> fired_events()
      const {
    return fired_;
  }

  // Appends one draw item per mesh of the prefab, placed by `root` (prefab
  // space -> world). `submeshes`, when not empty, replaces the prefab's
  // resolved submesh draws per mesh (World uses it for material overrides);
  // it must outlive the frame like the cache does.
  void AppendDraws(const glm::mat4& root, std::vector<render::DrawItem>& out,
                   std::span<const std::span<const render::SubmeshDraw>>
                       submeshes = {}) const;

  // Returns the prefab-space matrix of socket `name`, or nullopt.
  [[nodiscard]] std::optional<glm::mat4> SocketMatrix(
      std::string_view name) const;
  // Returns the prefab-space matrix of node `node` (-1 = identity).
  [[nodiscard]] glm::mat4 NodeMatrix(int node) const;

  [[nodiscard]] PrefabId prefab() const { return prefab_; }
  [[nodiscard]] int clip() const { return clip_; }
  [[nodiscard]] const animation::ClipPlayer& player() const { return player_; }
  [[nodiscard]] std::span<const glm::mat4> node_matrices() const {
    return world_;
  }

 private:
  // Samples the current clip at the player's time into the matrices.
  void Pose();
  // Collects events crossed between `from` and the player's time.
  void CollectEvents(float from);

  const PrefabCache* cache_;
  PrefabId prefab_;
  int clip_ = -1;
  animation::ClipPlayer player_;
  std::vector<animation::Transform> local_;
  std::vector<glm::mat4> world_;
  std::vector<const animation::ClipEvent*> fired_;
  // The next Update() also fires events at the start time.
  bool include_start_ = false;
};

}  // namespace blue::world

#endif  // BLUE_WORLD_PREFAB_POSE_H_
