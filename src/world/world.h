#ifndef BLUE_WORLD_WORLD_H_
#define BLUE_WORLD_WORLD_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "render/scene_view.h"
#include "scene/scene_data.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

// The runtime form of a scene: every instance of a scene::SceneData resolved
// into posed prefabs (nested child prefabs included), with material overrides
// applied, world matrices, gathered point lights and ready draw items. Shared
// by the game and the editor.

namespace blue::world {

// A problem met while building a world; the affected instance (or nested
// prefab) is skipped, the rest of the world is built.
struct WorldIssue {
  scene::InstanceId instance = scene::kNoInstance;
  std::string message;
};

// Nested prefabs deeper than this are dropped with an issue.
inline constexpr int kMaxPrefabDepth = 16;

// One resolved prefab copy: a scene instance's prefab, or a child prefab
// nested in one. `owner` is the scene instance it belongs to.
struct Placement {
  scene::InstanceId owner = scene::kNoInstance;
  PrefabId prefab{};
  // Index into World::placements() of the prefab this one is nested in, or
  // -1 for the instance's own prefab.
  int parent = -1;
  int depth = 0;          // 0 for the instance's own prefab
  glm::mat4 world{1.0f};  // prefab space -> world
};

struct PickHit {
  scene::InstanceId instance = scene::kNoInstance;
  float distance = 0.0f;  // along the normalised ray
  glm::vec3 point{0.0f};
};

enum class PickMode {
  kBounds,     // world bounds of each placement's meshes (fast)
  kTriangles,  // the CPU triangles of the meshes (exact; editor clicks)
};

// The resolved scene. Borrows `cache`, which must outlive it. Not
// thread-safe; neither copyable nor movable (draw items point into it).
//
// Editing: Build() is a full rebuild from SceneData. It loads nothing that
// is already cached and allocates only vectors, so the editor calls it after
// every structural edit (add/remove/override/reparent): 300 instances of a
// cached prefab rebuild in about 5 ms in a Debug build (world_test.cc). While a
// transform is being dragged, SetInstanceTransform() updates only the matrices
// of that instance and its descendants.
class World {
 public:
  explicit World(PrefabCache& cache) : cache_(cache) {}
  World(const World&) = delete;
  World& operator=(const World&) = delete;

  // Resolves `scene` (prefab paths through the cache, loaded on demand).
  // Hidden instances and their descendants are skipped. Returns the problems
  // met: unloadable prefab, nested prefab cycle (named path chain), depth
  // limit, broken parent chain, override of a missing material. Logs the
  // build time.
  std::vector<WorldIssue> Build(const scene::SceneData& scene);

  // Updates the world matrices of instance `id` and everything below it
  // after its transform (or a parent's) changed in `scene`; `scene` must be
  // the one passed to the last Build() apart from transforms. Returns false
  // if `id` is not in the world (hidden or unknown).
  bool SetInstanceTransform(const scene::SceneData& scene,
                            scene::InstanceId id);

  // Removes instance `id` and its descendants from the running world until
  // the next Build(): their placements draw nothing, their lights get zero
  // intensity, Pick(), InstanceBounds() and SocketMatrix() skip them and
  // AppendSelectedDraws() counts no draws for them. The scene data is not
  // involved (the game uses this for picked-up items; rebuilding brings
  // them back). One pass over the instances after `id` and the hidden
  // placements' lights; no allocation. Returns false if `id` is not in the
  // world; hiding a hidden instance is a no-op that returns true.
  bool HideInstance(scene::InstanceId id);
  // Returns true if instance `id` was hidden by HideInstance() (itself or
  // through an ancestor) since the last Build().
  [[nodiscard]] bool IsInstanceHidden(scene::InstanceId id) const;

  // Overrides the world matrix of instance `id`'s own prefab (dynamic
  // physics bodies; see WorldPhysics::SyncToWorld()). Its nested prefabs
  // follow. Returns false if `id` is not in the world.
  bool SetInstanceMatrix(scene::InstanceId id, const glm::mat4& world);

  // Starts clip `clip_name` on instance `id`'s own prefab. Returns false if
  // the instance or clip does not exist.
  bool PlayClip(scene::InstanceId id, std::string_view clip_name,
                float speed = 1.0f, std::optional<bool> loop = std::nullopt);
  // Returns the pose of instance `id`'s own prefab, or nullptr; the editor
  // scrubs it with SetTime(). Valid until the next Build().
  [[nodiscard]] PrefabPose* pose(scene::InstanceId id);

  // Advances the playing clips by `dt` seconds. Static worlds cost one
  // empty loop.
  void Update(float dt);

  // Appends the draw items of every visible placement. Does not allocate
  // once `out` has grown to its steady size.
  void AppendDraws(std::vector<render::DrawItem>& out) const;
  // Changes whenever AppendDraws() may append other items, or the same
  // ones in another order, than before (Build(), HideInstance()); while it
  // stays the same, the n-th item appended is the same mesh of the same
  // placement every frame. The key under which a DrawHistory follows the
  // world's draws from frame to frame. Never 0.
  [[nodiscard]] uint64_t draw_layout() const { return draw_layout_; }
  // Appends to `out` the indices (into the vector AppendDraws() appended to,
  // whose size was `first_draw` before) of the draws that belong to the
  // instances in `selection` or their descendants. Allocates nothing but
  // `out`'s growth.
  void AppendSelectedDraws(std::span<const scene::InstanceId> selection,
                           uint32_t first_draw,
                           std::vector<uint32_t>& out) const;

  // Scene point lights followed by every placement's prefab lights in world
  // space.
  [[nodiscard]] std::span<const render::PointLight> point_lights() const {
    return lights_;
  }

  [[nodiscard]] std::span<const Placement> placements() const {
    return placements_;
  }

  // Returns the world matrix of socket `name` of instance `id` (its own
  // prefab first, then its nested prefabs in order), or nullopt.
  [[nodiscard]] std::optional<glm::mat4> SocketMatrix(
      scene::InstanceId id, std::string_view name) const;

  // Returns the world-space box around every mesh of instance `id` and its
  // nested prefabs (not its child instances), or nullopt if it has none.
  [[nodiscard]] std::optional<asset::Bounds> InstanceBounds(
      scene::InstanceId id) const;

  // Returns the closest instance hit by the ray from `origin` along
  // `direction` (need not be normalised) within `max_distance`.
  [[nodiscard]] std::optional<PickHit> Pick(glm::vec3 origin,
                                            glm::vec3 direction,
                                            float max_distance,
                                            PickMode mode) const;

  // Returns, per entry of `scene.prefabs`, the material names of that prefab
  // and its nested prefabs (loaded through the cache), ready for
  // scene::ValidateAgainstPrefabs(). Unloadable prefabs get an empty list.
  [[nodiscard]] std::vector<std::vector<std::string>> MaterialNames(
      const scene::SceneData& scene);

  [[nodiscard]] PrefabCache& cache() { return cache_; }
  [[nodiscard]] const PrefabCache& cache() const { return cache_; }

 private:
  // Per placement: its pose, draw overrides and index range of its draws.
  struct PlacementState {
    PrefabPose pose;
    glm::mat4 local{1.0f};  // relative to the parent placement's node
    int parent_node = -1;   // node of the parent placement it hangs from
    // Per mesh: submesh draws with overrides applied (views into
    // override_draws_), or empty to use the prefab's own.
    std::vector<std::span<const render::SubmeshDraw>> submeshes;
    bool hidden = false;  // HideInstance(): drawn and lit no more
  };

  struct InstanceEntry {
    scene::InstanceId id = scene::kNoInstance;
    scene::InstanceId parent = scene::kNoInstance;
    glm::mat4 world{1.0f};
    int first_placement = 0;  // its own prefab's placement, or -1
    int placement_count = 0;  // own + nested
    bool hidden = false;      // HideInstance(), on itself or an ancestor
  };

  // Implementation in world.cc.
  struct Builder;
  friend struct Builder;

  // Recomputes the world matrices of the nested placements in [first,
  // first + count) from their parents (the first placement of an instance
  // keeps its own) and refreshes their lights.
  void UpdatePlacementMatrices(size_t first, size_t count);
  // Returns the number of placements nested below placement `index`.
  [[nodiscard]] size_t DescendantCount(size_t index) const;
  // Refreshes the prefab lights of the placements in [first, first+count).
  void UpdateLights(size_t first, size_t count);
  [[nodiscard]] const InstanceEntry* FindEntry(scene::InstanceId id) const;
  // Returns true if instance `id` is one of `roots` or below one of them.
  [[nodiscard]] bool IsInSubtree(
      scene::InstanceId id, std::span<const scene::InstanceId> roots) const;

  PrefabCache& cache_;
  std::vector<Placement> placements_;
  std::vector<PlacementState> states_;    // parallel to placements_
  std::vector<InstanceEntry> instances_;  // parents before children
  std::unordered_map<scene::InstanceId, size_t> entry_index_;
  // Override submesh draws of every placement, referenced by
  // PlacementState::submeshes.
  std::vector<render::SubmeshDraw> override_draws_;
  std::vector<render::PointLight> lights_;
  size_t scene_light_count_ = 0;
  // Per placement: index of its first light in lights_.
  std::vector<uint32_t> light_offsets_;
  // Placements with a playing clip (indices), rebuilt when clips start.
  std::vector<uint32_t> animated_;
  uint64_t draw_layout_ = 1;  // See draw_layout().
};

}  // namespace blue::world

#endif  // BLUE_WORLD_WORLD_H_
