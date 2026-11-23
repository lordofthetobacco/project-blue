#ifndef BLUE_INPUT_ACTION_STATE_H_
#define BLUE_INPUT_ACTION_STATE_H_

#include <bitset>
#include <cstddef>

#include "input/action.h"

namespace blue::input {

// Per-frame state of every action: whether it is held now and whether it went
// down or up at least once since the edges were last cleared. Edges are
// recorded on transitions of the held flag, so several keys bound to one
// action never double-fire and a press plus release inside one frame still
// reports both edges. Copyable.
class ActionState {
 public:
  [[nodiscard]] bool held(Action action) const { return held_[Index(action)]; }
  [[nodiscard]] bool pressed(Action action) const {
    return pressed_[Index(action)];
  }
  [[nodiscard]] bool released(Action action) const {
    return released_[Index(action)];
  }

  // Sets the held flag and records an edge if it changed.
  void SetHeld(Action action, bool down) {
    const size_t index = Index(action);
    if (held_[index] == down) {
      return;
    }
    held_[index] = down;
    (down ? pressed_ : released_)[index] = true;
  }

  // Forgets the edges, keeping what is held. Call once per frame.
  void ClearEdges() {
    pressed_.reset();
    released_.reset();
  }

  // Forgets `action`: not held, no edges (no release edge is recorded).
  void Forget(Action action) {
    const size_t index = Index(action);
    held_[index] = false;
    pressed_[index] = false;
    released_[index] = false;
  }

  // Forgets everything, without recording release edges.
  void Clear() {
    held_.reset();
    ClearEdges();
  }

  friend bool operator==(const ActionState&, const ActionState&) = default;

 private:
  static size_t Index(Action action) { return static_cast<size_t>(action); }

  std::bitset<kActionCount> held_;
  std::bitset<kActionCount> pressed_;
  std::bitset<kActionCount> released_;
};

}  // namespace blue::input

#endif  // BLUE_INPUT_ACTION_STATE_H_
