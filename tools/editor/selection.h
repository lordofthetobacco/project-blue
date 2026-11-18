#ifndef BLUE_EDITOR_SELECTION_H_
#define BLUE_EDITOR_SELECTION_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "scene/scene_data.h"

namespace blue::editor {

// What a selected item is. Prefab mode items are indices into the prefab's
// vectors; Scene mode items are instance ids or indices into the scene's
// vectors; the singletons (sun, environment, player start) have no index.
enum class SelectionKind : uint8_t {
  // Prefab mode.
  kNode,      // prefab::Prefab::hierarchy node
  kMesh,      // meshes
  kMaterial,  // materials
  kTexture,   // textures
  kSocket,    // sockets
  kLight,     // lights (prefab point lights)
  kChild,     // children (child prefabs)
  kClip,      // clips
  kCollider,  // the prefab's physics shape (no index)
  // Scene mode.
  kInstance,         // instances, by scene::InstanceId
  kScenePointLight,  // point_lights
  kPlayerStart,      // (no index)
  kSun,              // (no index)
  kEnvironment,      // (no index)
};

// One selected item: `kind` plus `index` (vector index, or the instance id
// for kInstance; -1 for the kinds without one).
struct SelectionItem {
  SelectionKind kind = SelectionKind::kNode;
  int64_t index = -1;

  friend bool operator==(const SelectionItem&, const SelectionItem&) = default;
};

// Returns the command-line name of `kind` ("node", "instance",
// "point-light", ...) and parses it back (nullopt for unknown names).
[[nodiscard]] const char* SelectionKindName(SelectionKind kind);
[[nodiscard]] std::optional<SelectionKind> ParseSelectionKind(
    std::string_view name);

[[nodiscard]] inline SelectionItem InstanceItem(scene::InstanceId id) {
  return {.kind = SelectionKind::kInstance, .index = int64_t{id}};
}

// The selection of one document, shared by its panels and the viewport. An
// ordered set: the last item added is the primary one (the inspector and the
// gizmo act on it). Not part of the undo history. Copyable. Not thread-safe.
class Selection {
 public:
  // Replaces the selection by `item`.
  void Set(SelectionItem item);
  // Adds `item` (moving it to primary if already selected).
  void Add(SelectionItem item);
  // Adds `item`, or removes it when selected (Ctrl+click).
  void Toggle(SelectionItem item);
  void Remove(SelectionItem item);
  void Clear() { items_.clear(); }

  [[nodiscard]] bool empty() const { return items_.empty(); }
  [[nodiscard]] bool Contains(SelectionItem item) const;
  // Returns the primary item, or nullopt when empty.
  [[nodiscard]] std::optional<SelectionItem> primary() const;
  // Returns the primary item's index if it is of `kind`, else -1.
  [[nodiscard]] int64_t PrimaryIndex(SelectionKind kind) const;
  // Oldest first; the primary item is last.
  [[nodiscard]] std::span<const SelectionItem> items() const { return items_; }
  // Returns the selected instance ids (Scene mode), in selection order.
  [[nodiscard]] std::vector<scene::InstanceId> Instances() const;
  // Drops selected instances that are not in `scene` any more (call after an
  // undo or a delete).
  void PruneInstances(const scene::SceneData& scene);
  // Drops items of `kind` whose index is >= `count`.
  void PruneIndices(SelectionKind kind, size_t count);

 private:
  std::vector<SelectionItem> items_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_SELECTION_H_
