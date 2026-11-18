#include "editor/editor_context.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "render/texture_cache.h"

namespace blue::editor {

namespace {

using VoidResult = core::Result<void, std::string>;

// Prefab cache key of the document preview: distinct from any file path, so
// a scene that places the same .pbo keeps the version on disk.
constexpr const char* kPreviewKey = "<editor prefab preview>";

}  // namespace

EditorContext::EditorContext(render::Renderer& renderer, ui::ImGuiLayer& imgui,
                             world::PrefabCache& prefab_cache,
                             std::string asset_root)
    : renderer_(renderer),
      imgui_(imgui),
      prefab_cache_(prefab_cache),
      asset_root_(core::NormalizePath(platform::AbsolutePath(asset_root))) {}

EditorContext::~EditorContext() { ResetPreview(); }

void EditorContext::SetMode(EditorMode mode) {
  if (mode_ != mode) {
    mode_ = mode;
    Status(mode == EditorMode::kPrefab ? "Prefab mode" : "Scene mode");
  }
}

Selection& EditorContext::selection() {
  if (mode_ == EditorMode::kPrefab && prefab_) {
    return prefab_->selection();
  }
  if (mode_ == EditorMode::kScene && scene_) {
    return scene_->selection();
  }
  empty_selection_.Clear();
  return empty_selection_;
}

VoidResult EditorContext::OpenFile(const std::string& path) {
  if (MatchesFileKind(path, FileKind::kPrefab)) {
    core::Result<PrefabDocument, std::string> loaded = LoadPrefabDocument(path);
    if (!loaded.ok()) {
      ShowError("Cannot open prefab", loaded.error());
      return VoidResult::Error(loaded.error());
    }
    auto document = std::make_shared<PrefabDocument>(std::move(loaded).value());
    ReplaceDocument(EditorMode::kPrefab, [this, document, path] {
      prefab_ = std::make_unique<PrefabDocument>(std::move(*document));
      SetMode(EditorMode::kPrefab);
      Status(std::format("Opened {}", DisplayPath(path)));
    });
    return VoidResult::Ok();
  }
  if (MatchesFileKind(path, FileKind::kScene)) {
    core::Result<SceneDocument, std::string> loaded = LoadSceneDocument(path);
    if (!loaded.ok()) {
      ShowError("Cannot open scene", loaded.error());
      return VoidResult::Error(loaded.error());
    }
    auto document = std::make_shared<SceneDocument>(std::move(loaded).value());
    ReplaceDocument(EditorMode::kScene, [this, document, path] {
      scene_ = std::make_unique<SceneDocument>(std::move(*document));
      SetMode(EditorMode::kScene);
      Status(std::format("Opened {}", DisplayPath(path)));
    });
    return VoidResult::Ok();
  }
  const std::string error = std::format(
      "{}: not a prefab (.pbo) or scene (.mscn); models are "
      "imported with File > Import model...",
      path);
  ShowError("Cannot open file", error);
  return VoidResult::Error(error);
}

void EditorContext::NewPrefabDocument(prefab::Prefab prefab, std::string path) {
  auto shared = std::make_shared<prefab::Prefab>(std::move(prefab));
  ReplaceDocument(EditorMode::kPrefab, [this, shared, path = std::move(path)] {
    prefab_ = std::make_unique<PrefabDocument>(std::move(*shared), path,
                                               /*dirty=*/true);
    SetMode(EditorMode::kPrefab);
  });
}

void EditorContext::NewSceneDocument() {
  ReplaceDocument(EditorMode::kScene, [this] {
    scene_ = std::make_unique<SceneDocument>(scene::SceneData{});
    SetMode(EditorMode::kScene);
  });
}

void EditorContext::SaveActive() {
  const bool untitled = mode_ == EditorMode::kPrefab
                            ? (prefab_ != nullptr && prefab_->path().empty())
                            : (scene_ != nullptr && scene_->path().empty());
  if (untitled) {
    SaveActiveAs();
  } else {
    SaveDocument(mode_, {});
  }
}

void EditorContext::SaveActiveAs() {
  const bool prefab_mode = mode_ == EditorMode::kPrefab;
  if (prefab_mode ? prefab_ == nullptr : scene_ == nullptr) {
    Status("Nothing to save");
    return;
  }
  const std::string current = prefab_mode ? prefab_->path() : scene_->path();
  const EditorMode mode = mode_;
  dialogs_.Save(prefab_mode ? FileKind::kPrefab : FileKind::kScene,
                current.empty() ? asset_root_ : current,
                [this, mode](std::string path) { SaveDocument(mode, path); });
}

bool EditorContext::SaveDocument(EditorMode mode, const std::string& path) {
  VoidResult saved = VoidResult::Ok();
  std::string saved_path;
  if (mode == EditorMode::kPrefab) {
    if (prefab_ == nullptr) {
      return false;
    }
    const std::string target = path.empty() ? prefab_->path() : path;
    saved = save_prefab_ ? save_prefab_(*prefab_, target)
                         : SavePrefabDocument(*prefab_, target);
    saved_path = prefab_->path();
  } else {
    if (scene_ == nullptr) {
      return false;
    }
    saved = SaveSceneDocument(*scene_, path);
    saved_path = scene_->path();
  }
  if (!saved.ok()) {
    ShowError("Save failed", saved.error());
    return false;
  }
  Status(std::format("Saved {}", DisplayPath(saved_path)));
  return true;
}

core::Result<void, std::string> EditorContext::SaveActiveTo(
    const std::string& path) {
  if (mode_ == EditorMode::kPrefab ? prefab_ == nullptr : scene_ == nullptr) {
    return VoidResult::Error("nothing to save");
  }
  if (!SaveDocument(mode_, path)) {
    return VoidResult::Error(std::format("saving {} failed", path));
  }
  return VoidResult::Ok();
}

core::Result<void, std::string> EditorContext::SaveAll() {
  for (const EditorMode mode : {EditorMode::kPrefab, EditorMode::kScene}) {
    const bool dirty = mode == EditorMode::kPrefab
                           ? prefab_ != nullptr && prefab_->dirty()
                           : scene_ != nullptr && scene_->dirty();
    if (!dirty) {
      continue;
    }
    const std::string& path =
        mode == EditorMode::kPrefab ? prefab_->path() : scene_->path();
    if (path.empty()) {
      const std::string error =
          "an untitled document has unsaved changes: use Save As first";
      ShowError("Save all", error);
      return VoidResult::Error(error);
    }
    if (!SaveDocument(mode, {})) {
      return VoidResult::Error(std::format("saving {} failed", path));
    }
  }
  return VoidResult::Ok();
}

bool EditorContext::Undo() {
  const bool undone = mode_ == EditorMode::kPrefab
                          ? prefab_ != nullptr && prefab_->Undo()
                          : scene_ != nullptr && scene_->Undo();
  if (undone) {
    PruneSelection();
  }
  return undone;
}

bool EditorContext::Redo() {
  const bool redone = mode_ == EditorMode::kPrefab
                          ? prefab_ != nullptr && prefab_->Redo()
                          : scene_ != nullptr && scene_->Redo();
  if (redone) {
    PruneSelection();
  }
  return redone;
}

void EditorContext::PruneSelection() {
  if (mode_ == EditorMode::kScene && scene_ != nullptr) {
    const scene::SceneData& state = scene_->state();
    Selection& selection = scene_->selection();
    selection.PruneInstances(state);
    selection.PruneIndices(SelectionKind::kScenePointLight,
                           state.point_lights.size());
    return;
  }
  if (prefab_ != nullptr) {
    const prefab::Prefab& state = prefab_->state();
    Selection& selection = prefab_->selection();
    selection.PruneIndices(SelectionKind::kNode, state.hierarchy.nodes.size());
    selection.PruneIndices(SelectionKind::kMesh, state.meshes.size());
    selection.PruneIndices(SelectionKind::kMaterial, state.materials.size());
    selection.PruneIndices(SelectionKind::kTexture, state.textures.size());
    selection.PruneIndices(SelectionKind::kSocket, state.sockets.size());
    selection.PruneIndices(SelectionKind::kLight, state.lights.size());
    selection.PruneIndices(SelectionKind::kChild, state.children.size());
    selection.PruneIndices(SelectionKind::kClip, state.clips.size());
  }
}

void EditorContext::ReplaceDocument(EditorMode mode,
                                    std::function<void()> proceed) {
  const bool dirty = mode == EditorMode::kPrefab
                         ? (prefab_ != nullptr && prefab_->dirty())
                         : (scene_ != nullptr && scene_->dirty());
  if (!dirty) {
    proceed();
    return;
  }
  const std::string& path =
      mode == EditorMode::kPrefab ? prefab_->path() : scene_->path();
  pending_replace_ = PendingReplace{.document_title = DocumentTitle(path),
                                    .mode = mode,
                                    .proceed = std::move(proceed)};
}

void EditorContext::ResolvePendingReplace(ReplaceChoice choice) {
  if (!pending_replace_) {
    return;
  }
  PendingReplace pending = std::move(*pending_replace_);
  pending_replace_.reset();
  switch (choice) {
    case ReplaceChoice::kSave: {
      const bool untitled = pending.mode == EditorMode::kPrefab
                                ? prefab_->path().empty()
                                : scene_->path().empty();
      if (untitled) {
        // Ask for a name; replace once it is saved.
        const EditorMode mode = pending.mode;
        dialogs_.Save(
            mode == EditorMode::kPrefab ? FileKind::kPrefab : FileKind::kScene,
            asset_root_,
            [this, mode,
             proceed = std::move(pending.proceed)](std::string path) {
              if (SaveDocument(mode, path)) {
                proceed();
              }
            });
      } else if (SaveDocument(pending.mode, {})) {
        pending.proceed();
      }
      break;
    }
    case ReplaceChoice::kDiscard:
      pending.proceed();
      break;
    case ReplaceChoice::kCancel:
      break;
  }
}

void EditorContext::SyncPrefabPreview() {
  if (prefab_ == nullptr) {
    ResetPreview();
    return;
  }
  const DocumentRevision revision = prefab_->revision();
  const bool same_document =
      preview_document_ == prefab_.get() && preview_id_.has_value();
  if (same_document && revision == preview_revision_) {
    return;
  }
  const prefab::Prefab& state = prefab_->state();
  if (same_document && revision.structure == preview_revision_.structure) {
    // Parameter-only change: material values in place, nothing uploaded.
    for (size_t i = 0; i < state.materials.size(); ++i) {
      prefab_cache_.UpdateMaterial(*preview_id_, static_cast<int>(i),
                                   state.materials[i]);
    }
    preview_revision_ = revision;
    return;
  }
  if (std::optional<std::string> problem = prefab::ValidatePrefab(state)) {
    // Keep showing the last valid version; say why it does not update.
    if (preview_revision_ != revision) {
      Status(std::format("Preview not updated: {}", *problem));
    }
    preview_revision_ = revision;
    preview_document_ = prefab_.get();
    return;
  }
  if (preview_id_) {
    prefab_cache_.Replace(*preview_id_, state);
  } else {
    preview_id_ = prefab_cache_.Upload(state, kPreviewKey);
  }
  preview_pose_ =
      std::make_unique<world::PrefabPose>(prefab_cache_, *preview_id_);
  preview_document_ = prefab_.get();
  preview_revision_ = revision;
}

void EditorContext::ResetPreview() {
  preview_pose_.reset();
  if (preview_id_) {
    prefab_cache_.Release(*preview_id_);
    preview_id_.reset();
  }
  preview_document_ = nullptr;
  preview_revision_ = {};
}

ImTextureID EditorContext::CreateImageTexture(int width, int height,
                                              std::span<const std::byte> rgba) {
  if (width <= 0 || height <= 0 ||
      rgba.size() != static_cast<size_t>(width) * height * 4) {
    core::log::Error("Thumbnail of {} x {} with {} bytes rejected", width,
                     height, rgba.size());
    return ImTextureID{};
  }
  const std::array<std::span<const std::byte>, 1> mips{rgba};
  const render::TextureUpload upload{
      .format = render::TextureFormat::kRgba8Srgb,
      .width = static_cast<uint32_t>(width),
      .height = static_cast<uint32_t>(height),
      .mips = mips,
      .name = "thumbnail",
  };
  const std::vector<render::TextureId> ids =
      renderer_.UploadTextures(std::span(&upload, 1));
  const ImTextureID texture =
      imgui_.AddTexture(renderer_.texture_view(ids.front()));
  image_textures_.push_back({texture, ids.front()});
  return texture;
}

void EditorContext::ReleaseImageTexture(ImTextureID texture) {
  for (size_t i = 0; i < image_textures_.size(); ++i) {
    if (image_textures_[i].imgui == texture) {
      imgui_.RemoveTexture(texture);
      const render::TextureId id = image_textures_[i].texture;
      renderer_.ReleaseTextures(std::span(&id, 1));
      image_textures_.erase(image_textures_.begin() +
                            static_cast<std::ptrdiff_t>(i));
      return;
    }
  }
}

std::string EditorContext::DisplayPath(const std::string& path) const {
  return editor::DisplayPath(core::NormalizePath(platform::AbsolutePath(path)),
                             asset_root_);
}

void EditorContext::ShowError(std::string title, std::string message) {
  core::log::Error("{}: {}", title, message);
  AddMessage(true, std::format("{}: {}", title, message));
  if (error_modals_) {
    errors_.push_back({std::move(title), std::move(message)});
  } else {
    status_ = std::format("{}: {}", title, message);
  }
}

void EditorContext::Status(std::string message) {
  core::log::Info("{}", message);
  AddMessage(false, message);
  status_ = std::move(message);
}

void EditorContext::AddMessage(bool error, std::string text) {
  if (messages_.size() >= kMaxMessages) {
    messages_.pop_front();
  }
  messages_.push_back({.serial = next_message_serial_++,
                       .error = error,
                       .text = std::move(text)});
}

void EditorContext::ImportModel() {
  if (import_model_) {
    import_model_();
  } else {
    Status("Import is not available (Prefab mode registered no importer)");
  }
}

}  // namespace blue::editor
