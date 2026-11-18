#ifndef BLUE_EDITOR_EDITOR_CONTEXT_H_
#define BLUE_EDITOR_EDITOR_CONTEXT_H_

#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "editor/commands.h"
#include "editor/dialogs.h"
#include "editor/document.h"
#include "editor/panel.h"
#include "editor/viewport/viewport_panel.h"
#include "prefab/prefab.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "ui/imgui_layer.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

namespace blue::editor {

// Handles File > Import model... (owned by Prefab mode).
using ImportModelHandler = std::function<void()>;
// Saves a prefab document to a path (owned by Prefab mode; the default is
// SavePrefabDocument). A returned error is shown in a modal.
using PrefabSaveHandler = std::function<core::Result<void, std::string>(
    PrefabDocument& document, const std::string& path)>;

// What panels see of the editor: the open documents, the mode, the
// renderer and the prefab cache, the command registry, dialogs, and the
// hand-over of the viewport's SceneView. Owned by EditorApp, handed to every
// Panel call; references obtained from it are valid for the current frame
// (documents may be replaced between frames, e.g. by File > Open).
//
// One prefab document and one scene document are open at a time; the mode
// decides which one the panels and the viewport work on. Neither copyable
// nor movable. Not thread-safe: main thread only (dialog callbacks are
// marshalled to it).
class EditorContext {
 public:
  EditorContext(render::Renderer& renderer, ui::ImGuiLayer& imgui,
                world::PrefabCache& prefab_cache, std::string asset_root);
  ~EditorContext();

  EditorContext(const EditorContext&) = delete;
  EditorContext& operator=(const EditorContext&) = delete;

  // --- Mode and documents ------------------------------------------------
  [[nodiscard]] EditorMode mode() const { return mode_; }
  void SetMode(EditorMode mode);

  // The open documents; null when none is open in that mode.
  [[nodiscard]] PrefabDocument* prefab_document() { return prefab_.get(); }
  [[nodiscard]] SceneDocument* scene_document() { return scene_.get(); }
  // The selection of the active mode's document (an empty dummy when none).
  [[nodiscard]] Selection& selection();

  // Opens `path` (.pbo -> Prefab mode, .mscn -> Scene mode, a model file ->
  // the import handler with that path preselected is not supported: models
  // go through File > Import model...). Asks about unsaved changes of the
  // document it replaces first (the open happens after the answer). Errors
  // are shown in a modal and returned.
  core::Result<void, std::string> OpenFile(const std::string& path);
  // Replaces the prefab document by a new one holding `prefab` (e.g. a
  // freshly cooked import), untitled unless `path` is given, dirty, and
  // switches to Prefab mode. Asks about unsaved changes first.
  void NewPrefabDocument(prefab::Prefab prefab, std::string path = {});
  // Replaces the scene document by an empty scene and switches to Scene mode.
  void NewSceneDocument();
  // Saves the active document to its path, or asks for one (Save As) when
  // untitled; errors are shown in a modal.
  void SaveActive();
  void SaveActiveAs();
  // Saves the active document to `path` now (no dialog); errors are shown
  // in a modal and returned.
  core::Result<void, std::string> SaveActiveTo(const std::string& path);
  // Saves every open dirty document that has a path; an untitled dirty one
  // is an error (it needs Save As).
  core::Result<void, std::string> SaveAll();
  // Undoes / redoes one step of the active document and drops selected
  // items that no longer exist; false when there was nothing to do.
  bool Undo();
  bool Redo();

  // --- Prefab preview (GPU side of the prefab document) ------------------
  // The prefab document uploaded to the prefab cache, refreshed by the
  // shell before panels run: a structural change re-uploads it (and
  // recreates prefab_pose()), a parameter-only change updates material
  // parameters in place. So edits the preview must show beyond material
  // values (node transforms, meshes, textures) are ChangeKind::kStructure;
  // socket / light / collider values are drawn by the modes from the
  // document itself. While the document fails prefab::ValidatePrefab() the
  // last valid version stays (with a status message). Empty without a prefab
  // document.
  [[nodiscard]] std::optional<world::PrefabId> prefab_preview() const {
    return preview_id_;
  }
  // The preview's pose (animation preview: Play / SetTime / Stop on it),
  // recreated when the prefab's structure changes. Null without a prefab
  // document.
  [[nodiscard]] world::PrefabPose* prefab_pose() { return preview_pose_.get(); }
  [[nodiscard]] world::PrefabCache& prefab_cache() { return prefab_cache_; }

  // --- Rendering -----------------------------------------------------------
  [[nodiscard]] render::Renderer& renderer() { return renderer_; }
  [[nodiscard]] ui::ImGuiLayer& imgui() { return imgui_; }
  // Set by the viewport panel during its Draw(): the view the frame renders
  // into the viewport image. `view` (and everything it points to) must stay
  // valid until the end of the frame. Without one the frame renders an
  // empty view.
  void SetFrameView(const render::SceneView* view) { frame_view_ = view; }
  [[nodiscard]] const render::SceneView* frame_view() const {
    return frame_view_;
  }
  // The viewport panel (created by EditorApp before the modes register):
  // modes set their ViewportClient on it and use its camera.
  [[nodiscard]] ViewportPanel& viewport() { return *viewport_; }
  void set_viewport(ViewportPanel* viewport) { viewport_ = viewport; }
  // Uploads an RGBA8 sRGB image (e.g. a texture thumbnail from
  // cook::DecodeTextureMip) and returns an ImGui texture for it.
  // `rgba` holds width * height * 4 bytes. GPU memory is only reclaimed at
  // exit (the renderer keeps textures resident); keep thumbnails small and
  // cached.
  [[nodiscard]] ImTextureID CreateImageTexture(int width, int height,
                                               std::span<const std::byte> rgba);
  // Stops showing a texture from CreateImageTexture() (the ImGui descriptor
  // is freed once no frame in flight uses it).
  void ReleaseImageTexture(ImTextureID texture);

  // --- Commands, dialogs, messages -----------------------------------------
  [[nodiscard]] CommandRegistry& commands() { return commands_; }
  [[nodiscard]] Dialogs& dialogs() { return dialogs_; }
  // Shows a modal with `message` (errors the user must see).
  void ShowError(std::string title, std::string message);
  // Shows `message` in the status bar (and logs it).
  void Status(std::string message);
  [[nodiscard]] const std::string& status() const { return status_; }
  // The recent status and error messages (Status(), ShowError()), oldest
  // first; at most kMaxMessages. Serials count every message from 0, so a
  // caller notes next_message_serial() before an action and reads the
  // messages from that serial on afterwards (e.g. the output of a command).
  struct Message {
    uint64_t serial = 0;
    bool error = false;
    std::string text;  // ShowError(): "<title>: <message>"
  };
  static constexpr size_t kMaxMessages = 256;
  [[nodiscard]] const std::deque<Message>& messages() const {
    return messages_;
  }
  [[nodiscard]] uint64_t next_message_serial() const {
    return next_message_serial_;
  }
  // While false, ShowError() shows its message in the status bar instead of
  // a modal (tool calls of the MCP server report errors to their client).
  void set_error_modals(bool enabled) { error_modals_ = enabled; }
  // The directory the asset browser starts in (<exe dir>/assets by default).
  [[nodiscard]] const std::string& asset_root() const { return asset_root_; }
  // Returns `path` relative to the asset root when inside it (for status
  // messages and labels), else unchanged.
  [[nodiscard]] std::string DisplayPath(const std::string& path) const;

  // --- Hooks for the mode panels -------------------------------------------
  void SetImportModelHandler(ImportModelHandler handler) {
    import_model_ = std::move(handler);
  }
  void SetPrefabSaveHandler(PrefabSaveHandler handler) {
    save_prefab_ = std::move(handler);
  }
  // Runs File > Import model... (logs when Prefab mode registered none).
  void ImportModel();

  // --- Shell internals (EditorApp) -----------------------------------------
  // Brings the prefab preview up to date with the document; call once per
  // frame before the panels.
  void SyncPrefabPreview();
  // Starts a frame: forgets the previous frame's view.
  void BeginFrame() {
    frame_view_ = nullptr;
    ++frame_count_;
  }
  // Frames begun so far.
  [[nodiscard]] uint64_t frame_count() const { return frame_count_; }
  // The pending error modal, shown and cleared by the shell.
  struct ErrorMessage {
    std::string title;
    std::string message;
  };
  [[nodiscard]] std::vector<ErrorMessage>& pending_errors() { return errors_; }
  // The question asked before replacing a dirty document: what replaces it
  // runs when the user chose Save (and it worked) or Discard.
  struct PendingReplace {
    std::string document_title;
    EditorMode mode = EditorMode::kPrefab;
    std::function<void()> proceed;
  };
  [[nodiscard]] const std::optional<PendingReplace>& pending_replace() const {
    return pending_replace_;
  }
  // The answer to pending_replace().
  enum class ReplaceChoice { kSave, kDiscard, kCancel };
  void ResolvePendingReplace(ReplaceChoice choice);

 private:
  // Runs `proceed` now if the document of `mode` is clean, otherwise asks
  // first (pending_replace_).
  void ReplaceDocument(EditorMode mode, std::function<void()> proceed);
  // Saves the document of `mode` to `path` (empty: its own); shows errors.
  bool SaveDocument(EditorMode mode, const std::string& path);
  void ResetPreview();
  // Drops selected items of the active document that no longer exist.
  void PruneSelection();
  void AddMessage(bool error, std::string text);

  render::Renderer& renderer_;
  ui::ImGuiLayer& imgui_;
  world::PrefabCache& prefab_cache_;
  std::string asset_root_;
  EditorMode mode_ = EditorMode::kPrefab;
  std::unique_ptr<PrefabDocument> prefab_;
  std::unique_ptr<SceneDocument> scene_;
  Selection empty_selection_;

  std::optional<world::PrefabId> preview_id_;
  std::unique_ptr<world::PrefabPose> preview_pose_;
  // The document revision the preview reflects.
  const PrefabDocument* preview_document_ = nullptr;
  DocumentRevision preview_revision_;

  const render::SceneView* frame_view_ = nullptr;
  uint64_t frame_count_ = 0;
  ViewportPanel* viewport_ = nullptr;  // Owned by EditorApp's panels.
  CommandRegistry commands_;
  Dialogs dialogs_;
  std::string status_;
  std::vector<ErrorMessage> errors_;
  std::deque<Message> messages_;
  uint64_t next_message_serial_ = 0;
  bool error_modals_ = true;
  std::optional<PendingReplace> pending_replace_;
  ImportModelHandler import_model_;
  PrefabSaveHandler save_prefab_;
  // Textures from CreateImageTexture(): the ImGui handle and the GPU one.
  struct ImageTexture {
    ImTextureID imgui{};
    render::TextureId texture{};
  };
  std::vector<ImageTexture> image_textures_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_EDITOR_CONTEXT_H_
