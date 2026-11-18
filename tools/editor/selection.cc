#include "editor/selection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "scene/scene_data.h"

namespace blue::editor {

namespace {

struct KindName {
  SelectionKind kind;
  const char* name;
};

constexpr KindName kKindNames[] = {
    {SelectionKind::kNode, "node"},
    {SelectionKind::kMesh, "mesh"},
    {SelectionKind::kMaterial, "material"},
    {SelectionKind::kTexture, "texture"},
    {SelectionKind::kSocket, "socket"},
    {SelectionKind::kLight, "light"},
    {SelectionKind::kChild, "child"},
    {SelectionKind::kClip, "clip"},
    {SelectionKind::kCollider, "collider"},
    {SelectionKind::kInstance, "instance"},
    {SelectionKind::kScenePointLight, "point-light"},
    {SelectionKind::kPlayerStart, "player-start"},
    {SelectionKind::kSun, "sun"},
    {SelectionKind::kEnvironment, "environment"},
};

}  // namespace

const char* SelectionKindName(SelectionKind kind) {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return "?";
}

std::optional<SelectionKind> ParseSelectionKind(std::string_view name) {
  for (const KindName& entry : kKindNames) {
    if (name == entry.name) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

void Selection::Set(SelectionItem item) {
  items_.clear();
  items_.push_back(item);
}

void Selection::Add(SelectionItem item) {
  Remove(item);
  items_.push_back(item);
}

void Selection::Toggle(SelectionItem item) {
  if (Contains(item)) {
    Remove(item);
  } else {
    items_.push_back(item);
  }
}

void Selection::Remove(SelectionItem item) { std::erase(items_, item); }

bool Selection::Contains(SelectionItem item) const {
  return std::find(items_.begin(), items_.end(), item) != items_.end();
}

std::optional<SelectionItem> Selection::primary() const {
  if (items_.empty()) {
    return std::nullopt;
  }
  return items_.back();
}

int64_t Selection::PrimaryIndex(SelectionKind kind) const {
  return !items_.empty() && items_.back().kind == kind ? items_.back().index
                                                       : -1;
}

std::vector<scene::InstanceId> Selection::Instances() const {
  std::vector<scene::InstanceId> ids;
  for (const SelectionItem& item : items_) {
    if (item.kind == SelectionKind::kInstance) {
      ids.push_back(static_cast<scene::InstanceId>(item.index));
    }
  }
  return ids;
}

void Selection::PruneInstances(const scene::SceneData& scene) {
  std::erase_if(items_, [&scene](const SelectionItem& item) {
    if (item.kind != SelectionKind::kInstance) {
      return false;
    }
    const auto id = static_cast<scene::InstanceId>(item.index);
    return std::none_of(
        scene.instances.begin(), scene.instances.end(),
        [id](const scene::Instance& instance) { return instance.id == id; });
  });
}

void Selection::PruneIndices(SelectionKind kind, size_t count) {
  std::erase_if(items_, [kind, count](const SelectionItem& item) {
    return item.kind == kind && item.index >= static_cast<int64_t>(count);
  });
}

}  // namespace blue::editor
