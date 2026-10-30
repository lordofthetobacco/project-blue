#include "platform/input_mapper.h"

#include <SDL3/SDL.h>

#include <utility>

#include "input/action.h"

namespace blue::platform {

namespace {

// 200 ms in SDL timestamp nanoseconds.
constexpr uint64_t kActivationClickWindow = 200'000'000;

[[nodiscard]] float Axis(bool positive, bool negative) {
  return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

}  // namespace

InputMapper::InputMapper(input::InputBindings bindings)
    : bindings_(std::move(bindings)) {}

void InputMapper::set_bindings(input::InputBindings bindings) {
  bindings_ = std::move(bindings);
  keys_down_.reset();
  buttons_down_.reset();
  actions_.Clear();
}

void InputMapper::set_mouse_mode(MouseMode mode) {
  if (mode == mouse_mode_) {
    return;
  }
  mouse_mode_ = mode;
  if (mode == MouseMode::kDrag) {
    mouse_captured_ = false;
  }
  motion_ = glm::vec2(0.0f);
}

void InputMapper::HandleEvent(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_QUIT:
      quit_requested_ = true;
      break;
    case SDL_EVENT_WINDOW_MINIMIZED:
      minimized_ = true;
      ReleaseInput();
      break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:  // Restoring a minimized maximized window
                                      // may report only this.
      minimized_ = false;
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      minimized_ = false;  // A focused window is not minimized.
      focused_ = true;
      focus_gained_at_ = event.common.timestamp;
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      // Never recapture on focus gain: the user may be alt-tabbing between
      // fullscreen apps.
      focused_ = false;
      ReleaseInput();
      break;
    default:
      break;
  }

  if (minimized_) {
    return;
  }

  switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      HandleKey(event);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      HandleMouseButton(event);
      break;
    case SDL_EVENT_MOUSE_MOTION:
      HandleMouseMotion(event);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      wheel_ += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y
                                                                : event.wheel.y;
      break;
    default:
      break;
  }
}

input::InputFrame InputMapper::EndFrame() {
  input::InputFrame frame;
  if (!minimized_) {
    frame.mouse_delta = motion_;
    frame.wheel = wheel_;
    frame.actions = actions_;
    if (mouse_mode_ == MouseMode::kCapture) {
      using input::Action;
      frame.move = {
          Axis(actions_.held(Action::kMoveRight),
               actions_.held(Action::kMoveLeft)),
          Axis(actions_.held(Action::kFlyUp), actions_.held(Action::kFlyDown)),
          Axis(actions_.held(Action::kMoveForward),
               actions_.held(Action::kMoveBack))};
      frame.sprint = actions_.held(Action::kSprint);
    }
    frame.look = LookActive();
    frame.pan = PanActive();
  }
  motion_ = glm::vec2(0.0f);
  wheel_ = 0.0f;
  actions_.ClearEdges();
  return frame;
}

void InputMapper::HandleKey(const SDL_Event& event) {
  const bool down = event.type == SDL_EVENT_KEY_DOWN;
  if (down && event.key.repeat) {
    return;
  }
  const SDL_Scancode scancode = event.key.scancode;
  if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT) {
    return;
  }
  keys_down_[scancode] = down;

  const bool escape_was_held = actions_.held(input::Action::kEscape);
  ApplyBinding(input::Binding::Key(scancode));
  if (!escape_was_held && actions_.held(input::Action::kEscape)) {
    Escape();
  }
}

void InputMapper::HandleMouseButton(const SDL_Event& event) {
  const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
  const int button = event.button.button;
  if (button < input::kMinMouseButton || button > input::kMaxMouseButton) {
    return;
  }
  // In capture mode a click on the free cursor only captures it: the click
  // is swallowed so that it does not also fire whatever is bound to Mouse1.
  if (mouse_mode_ == MouseMode::kCapture && !mouse_captured_) {
    if (down && button == SDL_BUTTON_LEFT && !IsActivationClick(event)) {
      CaptureMouse();
    }
    return;
  }
  buttons_down_[static_cast<size_t>(button)] = down;
  ApplyBinding(input::Binding::MouseButton(button));
}

void InputMapper::HandleMouseMotion(const SDL_Event& event) {
  if (!LookActive() && !PanActive()) {
    return;
  }
  if (discard_next_motion_) {
    discard_next_motion_ = false;
    return;
  }
  // Raw counts, deliberately not scaled by frame time: the camera turns by
  // delta * sensitivity.
  motion_ += glm::vec2(event.motion.xrel, event.motion.yrel);
}

void InputMapper::ApplyBinding(const input::Binding& binding) {
  for (const input::Action action : input::AllActions()) {
    if (!bindings_.IsBound(action, binding)) {
      continue;
    }
    bool held = false;
    for (const input::Binding& candidate : bindings_.For(action)) {
      held = held || IsDown(candidate);
    }
    actions_.SetHeld(action, held);
  }
}

bool InputMapper::IsDown(const input::Binding& binding) const {
  if (binding.kind == input::BindingKind::kMouseButton) {
    return ButtonDown(binding.code);
  }
  return binding.code > SDL_SCANCODE_UNKNOWN &&
         binding.code < SDL_SCANCODE_COUNT &&
         keys_down_[static_cast<size_t>(binding.code)];
}

void InputMapper::Escape() {
  if (mouse_mode_ == MouseMode::kCapture && mouse_captured_) {
    mouse_captured_ = false;
  } else {
    quit_requested_ = true;
  }
}

void InputMapper::CaptureMouse() {
  if (!mouse_captured_) {
    mouse_captured_ = true;
    discard_next_motion_ = true;
  }
}

void InputMapper::ReleaseInput() {
  mouse_captured_ = false;
  keys_down_.reset();
  buttons_down_.reset();
  actions_.Clear();
}

bool InputMapper::IsActivationClick(const SDL_Event& event) const {
  // The click that focuses the window only activates it. SDL's Windows backend
  // already swallows that click (unless SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH is
  // set), so this is the portable guarantee.
  return !focused_ ||
         event.button.timestamp - focus_gained_at_ < kActivationClickWindow;
}

bool InputMapper::ButtonDown(int button) const {
  return button >= input::kMinMouseButton &&
         button <= input::kMaxMouseButton &&
         buttons_down_[static_cast<size_t>(button)];
}

bool InputMapper::LookActive() const {
  return mouse_mode_ == MouseMode::kCapture ? mouse_captured_
                                            : ButtonDown(SDL_BUTTON_LEFT);
}

bool InputMapper::PanActive() const {
  return mouse_mode_ == MouseMode::kDrag &&
         (ButtonDown(SDL_BUTTON_RIGHT) || ButtonDown(SDL_BUTTON_MIDDLE));
}

}  // namespace blue::platform
