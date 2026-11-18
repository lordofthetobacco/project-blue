#include "editor/prefab_mode/prefab_mode_state.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "animation/hierarchy.h"
#include "asset/image_decoder.h"
#include "cook/clip_tools.h"
#include "cook/collision_cook.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "cook/texture_cook.h"
#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "prefab/prefab.h"

namespace blue::editor {

namespace {

[[nodiscard]] double NowSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Returns whether `index` addresses an element of `values`.
template <typename T>
[[nodiscard]] bool InRange(const std::vector<T>& values, int64_t index) {
  return index >= 0 && index < static_cast<int64_t>(values.size());
}

// Splits comma-separated names, trimming spaces; empty names are dropped.
[[nodiscard]] std::vector<std::string> SplitNames(std::string_view text) {
  std::vector<std::string> names;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view name = text.substr(start, end - start);
    while (!name.empty() && name.front() == ' ') {
      name.remove_prefix(1);
    }
    while (!name.empty() && name.back() == ' ') {
      name.remove_suffix(1);
    }
    if (!name.empty()) {
      names.emplace_back(name);
    }
    start = end + 1;
  }
  return names;
}

[[nodiscard]] std::string JoinNames(const std::vector<std::string>& names) {
  std::string text;
  for (const std::string& name : names) {
    if (!text.empty()) {
      text += ", ";
    }
    text += name;
  }
  return text;
}

// Re-cooks the collider of a re-imported prefab from its kept settings:
// primitives with their parameters, hulls and meshes from the new meshes.
[[nodiscard]] std::optional<std::string> RecookCollider(
    prefab::Prefab& prefab) {
  if (prefab.physics.kind == prefab::ColliderKind::kNone) {
    return std::nullopt;
  }
  const prefab::Physics settings = prefab.physics;
  const core::Result<void, cook::CookError> cooked =
      cook::CookCollision(prefab, settings, cook::ShapeFit::kParameters);
  if (!cooked.ok()) {
    return "collider re-cook failed: " + CookErrorText(cooked.error());
  }
  return std::nullopt;
}

}  // namespace

ImTextureID ThumbnailCache::Get(EditorContext& context,
                                const prefab::Texture& texture) {
  if (texture.mips.empty()) {
    return ImTextureID{};
  }
  const Key key{texture.mips.front().data.data(), texture.width, texture.height,
                static_cast<int>(texture.format)};
  if (const auto found = thumbnails_.find(key); found != thumbnails_.end()) {
    return found->second;
  }
  // The first level that fits kMaxSize, or the smallest one.
  int mip = 0;
  while (mip + 1 < texture.mip_count() &&
         std::max(texture.mips[static_cast<size_t>(mip)].width,
                  texture.mips[static_cast<size_t>(mip)].height) > kMaxSize) {
    ++mip;
  }
  const asset::ImageData image = cook::DecodeTextureMip(texture, mip);
  ImTextureID id{};
  if (!image.pixels.empty()) {
    id = context.CreateImageTexture(image.width, image.height,
                                    std::as_bytes(std::span(image.pixels)));
  }
  thumbnails_.emplace(key, id);
  return id;
}

BackgroundCook::~BackgroundCook() {
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool BackgroundCook::Start(std::string path, cook::ModelCookOptions options) {
  if (worker_.joinable()) {
    return false;
  }
  path_ = std::move(path);
  done_ = false;
  result_.reset();
  start_seconds_ = NowSeconds();
  worker_ = std::thread([this, path = path_, options = std::move(options)] {
    core::Result<prefab::Prefab, cook::CookError> cooked =
        cook::CookModel(path, options);
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      result_.emplace(std::move(cooked));
    }
    done_ = true;
  });
  return true;
}

float BackgroundCook::elapsed_seconds() const {
  return static_cast<float>(NowSeconds() - start_seconds_);
}

std::optional<core::Result<prefab::Prefab, cook::CookError>>
BackgroundCook::TakeResult() {
  if (!worker_.joinable() || !done_) {
    return std::nullopt;
  }
  worker_.join();
  const std::lock_guard<std::mutex> lock(mutex_);
  std::optional<core::Result<prefab::Prefab, cook::CookError>> result =
      std::move(result_);
  result_.reset();
  return result;
}

std::string CookErrorText(cook::CookError error) {
  return cook::ToString(error);
}

std::string ItemLabel(const prefab::Prefab& prefab, const SelectionItem& item) {
  const int64_t i = item.index;
  switch (item.kind) {
    case SelectionKind::kNode:
      return InRange(prefab.hierarchy.nodes, i)
                 ? "node " + prefab.hierarchy.nodes[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kMesh:
      return InRange(prefab.meshes, i)
                 ? "mesh " + prefab.meshes[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kMaterial:
      return InRange(prefab.materials, i)
                 ? "material " + prefab.materials[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kTexture:
      return InRange(prefab.textures, i)
                 ? "texture " + prefab.textures[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kSocket:
      return InRange(prefab.sockets, i)
                 ? "socket " + prefab.sockets[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kLight:
      return InRange(prefab.lights, i)
                 ? "light " + prefab.lights[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kChild:
      return InRange(prefab.children, i)
                 ? "child " + core::FileName(
                                  prefab.children[static_cast<size_t>(i)].path)
                 : std::string();
    case SelectionKind::kClip:
      return InRange(prefab.clips, i)
                 ? "clip " + prefab.clips[static_cast<size_t>(i)].name
                 : std::string();
    case SelectionKind::kCollider:
      return "collider";
    default:
      return std::string();
  }
}

std::optional<std::string> ItemName(const prefab::Prefab& prefab,
                                    const SelectionItem& item) {
  const int64_t i = item.index;
  const auto at = [i](const auto& values) -> const auto& {
    return values[static_cast<size_t>(i)];
  };
  switch (item.kind) {
    case SelectionKind::kNode:
      if (InRange(prefab.hierarchy.nodes, i)) {
        return at(prefab.hierarchy.nodes).name;
      }
      break;
    case SelectionKind::kMesh:
      if (InRange(prefab.meshes, i)) {
        return at(prefab.meshes).name;
      }
      break;
    case SelectionKind::kMaterial:
      if (InRange(prefab.materials, i)) {
        return at(prefab.materials).name;
      }
      break;
    case SelectionKind::kTexture:
      if (InRange(prefab.textures, i)) {
        return at(prefab.textures).name;
      }
      break;
    case SelectionKind::kSocket:
      if (InRange(prefab.sockets, i)) {
        return at(prefab.sockets).name;
      }
      break;
    case SelectionKind::kLight:
      if (InRange(prefab.lights, i)) {
        return at(prefab.lights).name;
      }
      break;
    case SelectionKind::kClip:
      if (InRange(prefab.clips, i)) {
        return at(prefab.clips).name;
      }
      break;
    default:
      break;
  }
  return std::nullopt;
}

std::optional<std::string> RenameItem(prefab::Prefab& prefab,
                                      const SelectionItem& item,
                                      std::string name) {
  if (name.empty()) {
    return std::string("the name is empty");
  }
  if (!ItemName(prefab, item)) {
    return std::string("this item has no name");
  }
  const size_t i = static_cast<size_t>(item.index);
  switch (item.kind) {
    case SelectionKind::kNode:
      prefab.hierarchy.nodes[i].name = std::move(name);
      break;
    case SelectionKind::kMesh:
      prefab.meshes[i].name = std::move(name);
      break;
    case SelectionKind::kMaterial:
      prefab.materials[i].name = std::move(name);
      break;
    case SelectionKind::kTexture:
      prefab.textures[i].name = std::move(name);
      break;
    case SelectionKind::kSocket:
      prefab.sockets[i].name = std::move(name);
      break;
    case SelectionKind::kLight:
      prefab.lights[i].name = std::move(name);
      break;
    case SelectionKind::kClip: {
      const core::Result<void, cook::CookError> renamed =
          cook::RenameClip(prefab, static_cast<int>(i), std::move(name));
      if (!renamed.ok()) {
        return CookErrorText(renamed.error());
      }
      break;
    }
    default:
      return std::string("this item has no name");
  }
  return std::nullopt;
}

std::optional<std::string> DeleteItem(prefab::Prefab& prefab,
                                      const SelectionItem& item) {
  const int64_t i = item.index;
  const auto erase = [i](auto& values) -> std::optional<std::string> {
    if (!InRange(values, i)) {
      return std::string("nothing to delete");
    }
    values.erase(values.begin() + static_cast<ptrdiff_t>(i));
    return std::nullopt;
  };
  switch (item.kind) {
    case SelectionKind::kNode:
      if (!RemoveNode(prefab, static_cast<int>(i))) {
        return std::string("nothing to delete");
      }
      UpdatePrefabBounds(prefab);
      return std::nullopt;
    case SelectionKind::kMesh: {
      std::optional<std::string> error = erase(prefab.meshes);
      if (!error) {
        UpdatePrefabBounds(prefab);
      }
      return error;
    }
    case SelectionKind::kMaterial: {
      if (!InRange(prefab.materials, i)) {
        return std::string("nothing to delete");
      }
      for (const prefab::Mesh& mesh : prefab.meshes) {
        for (const asset::Submesh& submesh : mesh.submeshes) {
          if (submesh.material == static_cast<int>(i)) {
            return std::format(
                "material \"{}\" is used by mesh \"{}\"; assign another "
                "material to its submeshes first",
                prefab.materials[static_cast<size_t>(i)].name, mesh.name);
          }
        }
      }
      for (prefab::Mesh& mesh : prefab.meshes) {
        for (asset::Submesh& submesh : mesh.submeshes) {
          if (submesh.material > static_cast<int>(i)) {
            --submesh.material;
          }
        }
      }
      return erase(prefab.materials);
    }
    case SelectionKind::kTexture:
      return RemoveTexture(prefab, static_cast<int>(i))
                 ? std::nullopt
                 : std::optional<std::string>("nothing to delete");
    case SelectionKind::kSocket:
      return erase(prefab.sockets);
    case SelectionKind::kLight:
      return erase(prefab.lights);
    case SelectionKind::kChild:
      return erase(prefab.children);
    case SelectionKind::kClip:
      return erase(prefab.clips);
    case SelectionKind::kCollider:
      prefab.physics = {};
      return std::nullopt;
    default:
      return std::string("nothing to delete");
  }
}

void RequestImport(PrefabModeShared& shared, std::string path) {
  ImportDialog& dialog = shared.import_dialog;
  dialog.open_requested = true;
  dialog.target = ImportTarget::kNewDocument;
  dialog.path = std::move(path);
  dialog.drop_nodes = JoinNames(dialog.options.drop_nodes);
}

void StartImportCook(EditorContext& context, PrefabModeShared& shared) {
  ImportDialog& dialog = shared.import_dialog;
  dialog.options.drop_nodes = SplitNames(dialog.drop_nodes);
  if (!shared.cook.Start(dialog.path, dialog.options)) {
    context.ShowError("Import model", "Another import is still cooking.");
    return;
  }
  shared.cook_target = dialog.target;
  context.Status(std::format("Cooking {}...", core::FileName(dialog.path)));
}

void FinishImportCook(EditorContext& context, PrefabModeShared& shared) {
  std::optional<core::Result<prefab::Prefab, cook::CookError>> result =
      shared.cook.TakeResult();
  if (!result) {
    return;
  }
  const std::string& source = shared.cook.path();
  if (!result->ok()) {
    context.ShowError("Import model",
                      std::format("Cannot import {}: {}", source,
                                  CookErrorText(result->error())));
    return;
  }
  prefab::Prefab fresh = std::move(*result).value();
  if (shared.cook_target == ImportTarget::kNewDocument) {
    context.Status(std::format("Imported {}", core::FileName(source)));
    context.NewPrefabDocument(std::move(fresh));
    return;
  }
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    return;
  }
  const bool done =
      TryEdit(context, *document, "Re-import", ChangeKind::kStructure,
              [&](prefab::Prefab& prefab) -> std::optional<std::string> {
                prefab = ReimportPrefab(prefab, std::move(fresh));
                return RecookCollider(prefab);
              });
  if (done) {
    document->selection().Clear();
    context.Status(std::format("Re-imported {}", core::FileName(source)));
  }
}

core::Result<void, std::string> SavePrefabChecked(PrefabDocument& document,
                                                  const std::string& path) {
  document.CommitEdit();
  if (std::optional<std::string> problem = CheckBeforeSave(document.state())) {
    return core::Result<void, std::string>::Error(
        "The prefab cannot be saved: " + *problem);
  }
  return SavePrefabDocument(document, path);
}

}  // namespace blue::editor
