#ifndef BLUE_EDITOR_PREFAB_MODE_PREFAB_MODE_STATE_H_
#define BLUE_EDITOR_PREFAB_MODE_PREFAB_MODE_STATE_H_

#include <imgui.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <glm/vec3.hpp>

#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "prefab/prefab.h"

// State shared by the Prefab mode panels, its viewport client and its
// commands: things that are not part of the document (and so not undone),
// such as the animation preview, cached texture thumbnails, the open modal
// dialogs and a model cook running in the background.

namespace blue::editor {

// Euler angles shown for a rotation while it is being dragged, so the
// fields do not jump between equivalent angle triples.
struct EulerCache {
  ImGuiID id = 0;
  glm::vec3 degrees{0.0f};
};

// The animation preview of the prefab document (drives
// EditorContext::prefab_pose()).
struct AnimationPreview {
  int clip = -1;  // clip shown on the timeline; -1 = none
  bool playing = false;
  float speed = 1.0f;
  bool loop = true;
  float time = 0.0f;  // seconds, the playhead
};

// Thumbnails of prefab textures for ImGui, keyed by the texture's identity
// (its first mip's bytes, which never change once stored), so undo and
// structural edits reuse them. Thumbnails are never released: the renderer
// keeps textures resident until exit, and each is at most kMaxSize square.
// Not thread-safe.
class ThumbnailCache {
 public:
  static constexpr int kMaxSize = 128;

  ThumbnailCache() = default;
  ThumbnailCache(const ThumbnailCache&) = delete;
  ThumbnailCache& operator=(const ThumbnailCache&) = delete;

  // Returns the thumbnail of `texture`, decoding and uploading it on first
  // use; a null id for a texture without mips.
  [[nodiscard]] ImTextureID Get(EditorContext& context,
                                const prefab::Texture& texture);

 private:
  using Key = std::tuple<const std::byte*, int, int, int>;
  std::map<Key, ImTextureID> thumbnails_;
};

// A model cook running on a worker thread so the UI keeps drawing. At most
// one runs at a time. Neither copyable nor movable; joins the thread on
// destruction.
class BackgroundCook {
 public:
  BackgroundCook() = default;
  ~BackgroundCook();
  BackgroundCook(const BackgroundCook&) = delete;
  BackgroundCook& operator=(const BackgroundCook&) = delete;

  // Starts cooking `path` with `options`; returns false if a cook runs.
  bool Start(std::string path, cook::ModelCookOptions options);
  // Whether a cook was started and its result not yet taken.
  [[nodiscard]] bool busy() const { return worker_.joinable(); }
  // Seconds since Start().
  [[nodiscard]] float elapsed_seconds() const;
  // Returns the finished result (joining the worker), or nullopt while
  // running or idle.
  [[nodiscard]] std::optional<core::Result<prefab::Prefab, cook::CookError>>
  TakeResult();
  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::thread worker_;
  std::atomic<bool> done_{false};
  std::mutex mutex_;
  std::optional<core::Result<prefab::Prefab, cook::CookError>> result_;
  std::string path_;
  double start_seconds_ = 0.0;
};

// What the model import dialog does once the cook finished.
enum class ImportTarget {
  kNewDocument,  // File > Import model...: a new untitled prefab
  kReimport,     // re-import into the open document (ReimportPrefab)
};

// The "Import model" modal: the source and options being edited.
struct ImportDialog {
  bool open_requested = false;
  ImportTarget target = ImportTarget::kNewDocument;
  std::string path;
  cook::ModelCookOptions options;
  std::string drop_nodes;  // comma-separated, edited as text
};

// The "Pack ORM texture" modal for one material.
struct OrmDialog {
  bool open_requested = false;
  int material = -1;
  OrmImportSources sources;
};

// The "Rename" modal for one selected item (node, mesh, material, socket,
// light, clip).
struct RenameDialog {
  bool open_requested = false;
  SelectionItem item;
  std::string name;
};

// Everything the Prefab mode parts share; owned by RegisterPrefabMode() and
// outliving the panels that point at it.
struct PrefabModeShared {
  AnimationPreview preview;
  ThumbnailCache thumbnails;
  BackgroundCook cook;
  // What the running cook is for (the dialog may be reopened meanwhile).
  ImportTarget cook_target = ImportTarget::kNewDocument;
  ImportDialog import_dialog;
  OrmDialog orm_dialog;
  RenameDialog rename_dialog;
  TextureImportSettings texture_settings;
  // The UV layout panel's mesh and set (also set by "Show UVs" buttons).
  int uv_mesh = 0;
  int uv_set = 0;
  // The viewport gizmo is dragging (an undo step is open on purpose).
  bool gizmo_dragging = false;
  EulerCache euler;
  // The text being typed into the active InputTextCommit() field.
  std::string text_buffer;
  ImGuiID text_buffer_id = 0;
};

// Applies `change` (a callable taking prefab::Prefab& and returning
// std::optional<std::string>, the error) to a copy of the document's prefab
// and, if it returns no error, makes the copy the new state as one undo step.
// Otherwise the document is untouched and the error is shown in a modal
// titled `label`. Returns true on success. Copies are cheap (bulk data is
// shared); meant for buttons and commands, not per-frame drags.
template <typename Change>
bool TryEdit(EditorContext& context, PrefabDocument& document,
             std::string_view label, ChangeKind kind, Change&& change) {
  prefab::Prefab copy = document.state();
  if (std::optional<std::string> error = std::forward<Change>(change)(copy)) {
    context.ShowError(std::string(label), std::move(*error));
    return false;
  }
  document.Edit(label, kind,
                [&](prefab::Prefab& prefab) { prefab = std::move(copy); });
  return true;
}

// Like TryEdit() but returns the error instead of showing it (commands).
template <typename Change>
core::Result<void, std::string> TryEditQuiet(PrefabDocument& document,
                                             std::string_view label,
                                             ChangeKind kind, Change&& change) {
  prefab::Prefab copy = document.state();
  if (std::optional<std::string> error = std::forward<Change>(change)(copy)) {
    return core::Result<void, std::string>::Error(std::move(*error));
  }
  document.Edit(label, kind,
                [&](prefab::Prefab& prefab) { prefab = std::move(copy); });
  return core::Result<void, std::string>::Ok();
}

// Returns the readable name of a cook error for messages.
[[nodiscard]] std::string CookErrorText(cook::CookError error);

// Returns the display name of selection `item` in `prefab` ("node slide",
// "socket muzzle", ...), or "" when its index is out of range.
[[nodiscard]] std::string ItemLabel(const prefab::Prefab& prefab,
                                    const SelectionItem& item);

// Returns the current name of `item`, or nullopt for kinds without a name.
[[nodiscard]] std::optional<std::string> ItemName(const prefab::Prefab& prefab,
                                                  const SelectionItem& item);

// Renames `item` in `prefab`; returns an error for an empty name, a bad index
// or a kind without a name. Clip renames go through cook::RenameClip.
[[nodiscard]] std::optional<std::string> RenameItem(prefab::Prefab& prefab,
                                                    const SelectionItem& item,
                                                    std::string name);

// Deletes `item` from `prefab` (node subtree, mesh, material if unused,
// texture, socket, light, child, clip; the collider becomes kNone); returns
// an error for a bad index or a material still in use.
[[nodiscard]] std::optional<std::string> DeleteItem(prefab::Prefab& prefab,
                                                    const SelectionItem& item);

// Opens the import dialog for a new document from `path`, with the options
// the user last chose.
void RequestImport(PrefabModeShared& shared, std::string path);

// Starts the cook the import dialog describes (closing it); errors go to a
// modal.
void StartImportCook(EditorContext& context, PrefabModeShared& shared);

// Takes a finished background cook and applies it (new document or
// re-import); call once per frame.
void FinishImportCook(EditorContext& context, PrefabModeShared& shared);

// Saves `document` to `path` after CheckBeforeSave(); the Prefab mode save
// handler.
[[nodiscard]] core::Result<void, std::string> SavePrefabChecked(
    PrefabDocument& document, const std::string& path);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_PREFAB_MODE_PREFAB_MODE_STATE_H_
