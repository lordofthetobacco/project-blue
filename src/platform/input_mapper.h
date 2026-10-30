#ifndef BLUE_PLATFORM_INPUT_MAPPER_H_
#define BLUE_PLATFORM_INPUT_MAPPER_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_scancode.h>

#include <bitset>
#include <cstdint>

#include <glm/vec2.hpp>

#include "input/action_state.h"
#include "input/bindings.h"
#include "input/input_frame.h"

namespace blue::platform {

// Interprets SDL events through a table of key bindings into per-frame action
// state and mouse deltas. Pure logic over event data: it never calls into
// SDL, so it works without a window. Side effects (capturing the cursor,
// quitting) are exposed as requests. Copyable. Not thread-safe.
class InputMapper {
 public:
  enum class MouseMode {
    // Player and fly camera: a click captures the cursor, captured motion is
    // look, mouse buttons feed their bindings once captured.
    kCapture,
    // Orbit camera: the cursor is never captured; dragging with the left
    // button looks, with the right or middle button pans.
    kDrag,
  };

  InputMapper() = default;
  explicit InputMapper(input::InputBindings bindings);
  InputMapper(const InputMapper&) = default;
  InputMapper& operator=(const InputMapper&) = default;
  InputMapper(InputMapper&&) = default;
  InputMapper& operator=(InputMapper&&) = default;

  // Replaces the bindings. Everything held is released without edges.
  void set_bindings(input::InputBindings bindings);
  [[nodiscard]] const input::InputBindings& bindings() const {
    return bindings_;
  }

  // Switching to kDrag releases the capture; both drop pending motion.
  void set_mouse_mode(MouseMode mode);
  [[nodiscard]] MouseMode mouse_mode() const { return mouse_mode_; }

  // Feeds one SDL event into the mapper. Events other than window state are
  // ignored while the window is minimized.
  void HandleEvent(const SDL_Event& event);

  // Returns the input accumulated since the previous call, then resets the
  // per-frame sums and action edges.
  [[nodiscard]] input::InputFrame EndFrame();

  [[nodiscard]] bool wants_mouse_captured() const { return mouse_captured_; }
  [[nodiscard]] bool quit_requested() const { return quit_requested_; }

 private:
  void HandleKey(const SDL_Event& event);
  void HandleMouseButton(const SDL_Event& event);
  void HandleMouseMotion(const SDL_Event& event);
  // Recomputes the held flag of every action `binding` belongs to.
  void ApplyBinding(const input::Binding& binding);
  [[nodiscard]] bool IsDown(const input::Binding& binding) const;
  void Escape();
  void CaptureMouse();
  void ReleaseInput();
  [[nodiscard]] bool IsActivationClick(const SDL_Event& event) const;
  [[nodiscard]] bool ButtonDown(int button) const;
  [[nodiscard]] bool LookActive() const;
  [[nodiscard]] bool PanActive() const;

  input::InputBindings bindings_;
  MouseMode mouse_mode_ = MouseMode::kCapture;
  std::bitset<SDL_SCANCODE_COUNT> keys_down_;
  std::bitset<input::kMaxMouseButton + 1> buttons_down_;
  input::ActionState actions_;
  glm::vec2 motion_{0.0f};
  float wheel_ = 0.0f;

  uint64_t focus_gained_at_ = 0;  // SDL event timestamp, nanoseconds.
  bool focused_ = false;          // Unfocused until the first FOCUS_GAINED.
  bool mouse_captured_ = false;
  // SDL can report a jump when it warps the cursor on capture.
  bool discard_next_motion_ = false;
  bool minimized_ = false;
  bool quit_requested_ = false;
};

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_INPUT_MAPPER_H_
