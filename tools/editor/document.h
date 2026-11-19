#ifndef BLUE_EDITOR_DOCUMENT_H_
#define BLUE_EDITOR_DOCUMENT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "prefab/prefab.h"
#include "scene/scene_data.h"

namespace blue::editor {

// Counters that advance with every change of a document, so the GPU side
// (world upload, scene World) refreshes only when and as much as needed:
// compare with the values seen last frame. `structure` advances on
// structural changes only; `any` on every change (parameters included).
struct DocumentRevision {
  uint64_t any = 0;
  uint64_t structure = 0;

  friend bool operator==(const DocumentRevision&,
                         const DocumentRevision&) = default;
};

// An open file being edited: its state (a whole prefab or scene), where it
// is saved, whether it has unsaved changes, its undo history and selection.
//
// Every change goes through an edit, so it can be undone:
//
//   // One step; e.g. a button.
//   doc.Edit("Add socket", ChangeKind::kStructure,
//            [](prefab::Prefab& p) { p.sockets.push_back({...}); });
//
//   // One step over several frames; e.g. a slider drag.
//   float roughness = doc.state().materials[i].roughness;
//   if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f)) {
//     doc.BeginEdit("Roughness", ChangeKind::kParameters)
//         .materials[i].roughness = roughness;
//   }
//   if (ImGui::IsItemDeactivatedAfterEdit()) doc.CommitEdit();
//
// BeginEdit() returns the state to modify; the first call snapshots it for
// undo, later calls before CommitEdit() continue the same step. Every call
// advances revision(). Copyable. Not thread-safe.
template <typename State>
class Document {
 public:
  // A document holding `state`, saved at `path` (empty: untitled, Save
  // needs a path), with no unsaved changes unless `dirty`.
  explicit Document(State state, std::string path = {}, bool dirty = false)
      : state_(std::move(state)), path_(std::move(path)), dirty_(dirty) {}

  [[nodiscard]] const State& state() const { return state_; }

  // Starts or continues an edit (see the class comment) and returns the
  // state to change. Marks the document dirty and advances the revision.
  [[nodiscard]] State& BeginEdit(std::string_view label, ChangeKind kind) {
    history_.Begin(state_, label, kind);
    Touch(kind);
    return state_;
  }
  // Ends the open edit as one undo step; no-op without one.
  void CommitEdit() { history_.Commit(); }
  // Ends the open edit and restores the state from before it.
  void CancelEdit() {
    if (std::optional<State> before = history_.Cancel()) {
      state_ = std::move(*before);
      Touch(ChangeKind::kStructure);
    }
  }
  // One complete edit: BeginEdit(label, kind), `change(state)`, CommitEdit().
  template <typename Change>
  void Edit(std::string_view label, ChangeKind kind, Change&& change) {
    std::forward<Change>(change)(BeginEdit(label, kind));
    CommitEdit();
  }

  // Undoes / redoes one step; returns false when there is none.
  bool Undo() { return Apply(history_.Undo(state_)); }
  bool Redo() { return Apply(history_.Redo(state_)); }
  [[nodiscard]] const SnapshotHistory<State>& history() const {
    return history_;
  }
  // Drops the oldest undo step (e.g. to bound the memory the history holds);
  // returns false when there is none.
  bool DropOldestUndoStep() { return history_.DropOldest(); }

  // Calls `rewrite(State&)`, which must change how the state is represented
  // but not its value (e.g. point bulk data at an equal copy): no undo step,
  // the revision and the dirty flag stay.
  template <typename Rewrite>
  void RewriteState(Rewrite&& rewrite) {
    std::forward<Rewrite>(rewrite)(state_);
  }

  // Where the document is saved; empty while untitled.
  [[nodiscard]] const std::string& path() const { return path_; }
  void set_path(std::string path) { path_ = std::move(path); }
  [[nodiscard]] bool dirty() const { return dirty_; }
  // Records that the current state is what is on disk at path().
  void MarkSaved() { dirty_ = false; }
  [[nodiscard]] DocumentRevision revision() const { return revision_; }

  [[nodiscard]] Selection& selection() { return selection_; }
  [[nodiscard]] const Selection& selection() const { return selection_; }

 private:
  void Touch(ChangeKind kind) {
    dirty_ = true;
    ++revision_.any;
    if (kind == ChangeKind::kStructure) {
      ++revision_.structure;
    }
  }
  bool Apply(std::optional<ChangeKind> kind) {
    if (!kind) {
      return false;
    }
    Touch(*kind);
    return true;
  }

  State state_;
  std::string path_;
  bool dirty_ = false;
  DocumentRevision revision_;
  SnapshotHistory<State> history_;
  Selection selection_;
};

using PrefabDocument = Document<prefab::Prefab>;
using SceneDocument = Document<scene::SceneData>;

// Loads the .pbo at `path` into a clean document; the error is readable
// ("<path>: <reason>").
[[nodiscard]] core::Result<PrefabDocument, std::string> LoadPrefabDocument(
    const std::string& path);
// Validates the prefab (prefab::ValidatePrefab) and writes it to `path`
// (empty: the document's path, which must be set). On success the document
// takes `path` and is clean, and its bulk data is re-pointed at the saved
// file (mapped; prefab::RebaseBulk), so cooked or edited data stops taking
// heap memory once saved; undo steps keep what they reference. On failure it
// is unchanged and the error is readable. An open edit is committed first.
[[nodiscard]] core::Result<void, std::string> SavePrefabDocument(
    PrefabDocument& document, const std::string& path = {});

// The .mscn counterparts of the functions above (scene::CheckMscnSerializable
// before writing).
[[nodiscard]] core::Result<SceneDocument, std::string> LoadSceneDocument(
    const std::string& path);
[[nodiscard]] core::Result<void, std::string> SaveSceneDocument(
    SceneDocument& document, const std::string& path = {});

// Returns the file name of `path` for titles and tabs, or "Untitled".
[[nodiscard]] std::string DocumentTitle(const std::string& path);

// Returns `path` for display: relative to `root` (the asset root) when it
// lies inside it, else `path` unchanged. Both should be absolute.
[[nodiscard]] std::string DisplayPath(const std::string& path,
                                      const std::string& root);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_DOCUMENT_H_
