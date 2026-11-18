#ifndef BLUE_EDITOR_UNDO_H_
#define BLUE_EDITOR_UNDO_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blue::editor {

// What an edit changed, so the GPU copy of a document is refreshed only as
// much as needed: parameter changes (material values, light colours, socket
// transforms, instance transforms) are cheap to apply, structural changes
// (meshes, textures, hierarchy, instances added or removed) mean a re-upload
// or rebuild.
enum class ChangeKind : uint8_t {
  kParameters,
  kStructure,
};

// Snapshot-based undo history of one value of type `State` (a whole
// document). An edit is bracketed by Begin() and Commit(): Begin() remembers
// the state before the edit, Commit() turns it into an undo step. Repeated
// Begin() calls while an edit is open are ignored, so a slider drag that
// calls Begin() every frame and Commit() on release is one step. The history
// keeps at most `capacity` undo steps, dropping the oldest. `State` must be
// copyable; copies should be cheap (prefab bulk data is shared). Copyable.
// Not thread-safe.
template <typename State>
class SnapshotHistory {
 public:
  static constexpr size_t kDefaultCapacity = 200;

  explicit SnapshotHistory(size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  // Opens an edit of `current` labelled `label` (shown as "Undo <label>")
  // unless one is open; a structural `kind` upgrades an open parameter edit.
  void Begin(const State& current, std::string_view label, ChangeKind kind) {
    if (open_) {
      if (kind == ChangeKind::kStructure) {
        open_->kind = kind;
      }
      return;
    }
    open_ = Step{current, std::string(label), kind};
  }

  // Closes the open edit as one undo step and clears the redo steps. No-op
  // without an open edit.
  void Commit() {
    if (!open_) {
      return;
    }
    undo_.push_back(std::move(*open_));
    open_.reset();
    redo_.clear();
    while (undo_.size() > capacity_) {
      undo_.pop_front();
    }
  }

  // Closes the open edit and returns the state from before it (the caller
  // restores it), or nullopt without an open edit.
  [[nodiscard]] std::optional<State> Cancel() {
    if (!open_) {
      return std::nullopt;
    }
    std::optional<State> before(std::move(open_->state));
    open_.reset();
    return before;
  }

  // Replaces `current` by the state before the newest undo step and returns
  // that step's change kind, or nullopt when there is nothing to undo. An
  // open edit is committed first.
  [[nodiscard]] std::optional<ChangeKind> Undo(State& current) {
    Commit();
    if (undo_.empty()) {
      return std::nullopt;
    }
    Step step = std::move(undo_.back());
    undo_.pop_back();
    std::swap(current, step.state);
    const ChangeKind kind = step.kind;
    redo_.push_back(std::move(step));  // Now holds the state undone.
    return kind;
  }

  // Re-applies the newest undone step to `current`; see Undo().
  [[nodiscard]] std::optional<ChangeKind> Redo(State& current) {
    Commit();
    if (redo_.empty()) {
      return std::nullopt;
    }
    Step step = std::move(redo_.back());
    redo_.pop_back();
    std::swap(current, step.state);
    const ChangeKind kind = step.kind;
    undo_.push_back(std::move(step));
    return kind;
  }

  // Drops every step and any open edit.
  void Clear() {
    undo_.clear();
    redo_.clear();
    open_.reset();
  }

  [[nodiscard]] bool can_undo() const { return !undo_.empty() || open_; }
  [[nodiscard]] bool can_redo() const { return !redo_.empty(); }
  [[nodiscard]] bool edit_open() const { return open_.has_value(); }
  [[nodiscard]] size_t undo_count() const { return undo_.size(); }
  [[nodiscard]] size_t redo_count() const { return redo_.size(); }
  // Labels of the steps Undo() / Redo() would apply; empty when none.
  [[nodiscard]] std::string_view undo_label() const {
    return open_           ? std::string_view(open_->label)
           : undo_.empty() ? std::string_view()
                           : std::string_view(undo_.back().label);
  }
  [[nodiscard]] std::string_view redo_label() const {
    return redo_.empty() ? std::string_view()
                         : std::string_view(redo_.back().label);
  }

 private:
  struct Step {
    State state;
    std::string label;
    ChangeKind kind = ChangeKind::kParameters;
  };

  size_t capacity_;
  std::deque<Step> undo_;
  std::vector<Step> redo_;
  std::optional<Step> open_;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_UNDO_H_
