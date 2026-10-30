#include "platform/input_mapper.h"

#include <SDL3/SDL.h>

#include <utility>

namespace blue::platform {

namespace {

// 200 ms in SDL timestamp nanoseconds.
constexpr uint64_t kActivationClickWindow = 200'000'000;

[[nodiscard]] float Axis(bool positive, bool negative) {
  return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

}  // namespace

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
    if (mode_ == camera::CameraMode::kFly) {
      frame.move = {Axis(keys_.right, keys_.left), Axis(keys_.up, keys_.down),
                    Axis(keys_.forward, keys_.back)};
      frame.sprint = keys_.sprint;
    }
    frame.look = LookActive();
    frame.pan = PanActive();
  }
  motion_ = glm::vec2(0.0f);
  wheel_ = 0.0f;
  return frame;
}

std::vector<InputAction> InputMapper::TakeActions() {
  return std::exchange(actions_, {});
}

void InputMapper::HandleKey(const SDL_Event& event) {
  const bool down = event.type == SDL_EVENT_KEY_DOWN;
  SetHeldKey(event.key.scancode, down);
  if (!down || event.key.repeat) {
    return;
  }

  switch (event.key.key) {
    case SDLK_ESCAPE:
      Escape();
      break;
    case SDLK_TAB:
      ToggleMode();
      break;
    case SDLK_I:
      actions_.push_back(InputAction::kToggleInvertY);
      break;
    case SDLK_LEFTBRACKET:
      actions_.push_back(InputAction::kSensitivityDown);
      break;
    case SDLK_RIGHTBRACKET:
      actions_.push_back(InputAction::kSensitivityUp);
      break;
    case SDLK_F1:
      actions_.push_back(InputAction::kToggleOverlay);
      break;
    case SDLK_F5:
      actions_.push_back(InputAction::kSaveScene);
      break;
    case SDLK_F9:
      actions_.push_back(InputAction::kLoadScene);
      break;
    case SDLK_F:
      actions_.push_back(InputAction::kPush);
      break;
    case SDLK_V:
      actions_.push_back(InputAction::kToggleVsync);
      break;
    case SDLK_C:
      actions_.push_back(InputAction::kToggleCascadeDebug);
      break;
    case SDLK_X:
      actions_.push_back(InputAction::kToggleShadows);
      break;
    default:
      break;
  }
}

void InputMapper::HandleMouseButton(const SDL_Event& event) {
  const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
  SetHeldButton(event.button.button, down);
  if (down && event.button.button == SDL_BUTTON_LEFT &&
      mode_ == camera::CameraMode::kFly && !IsActivationClick(event)) {
    CaptureMouse();
  }
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

void InputMapper::SetHeldKey(int scancode, bool down) {
  switch (scancode) {
    case SDL_SCANCODE_W:
      keys_.forward = down;
      break;
    case SDL_SCANCODE_S:
      keys_.back = down;
      break;
    case SDL_SCANCODE_A:
      keys_.left = down;
      break;
    case SDL_SCANCODE_D:
      keys_.right = down;
      break;
    case SDL_SCANCODE_SPACE:
      keys_.up = down;
      break;
    case SDL_SCANCODE_LCTRL:
      keys_.down = down;
      break;
    case SDL_SCANCODE_LSHIFT:
      keys_.sprint = down;
      break;
    default:
      break;
  }
}

void InputMapper::SetHeldButton(int button, bool down) {
  switch (button) {
    case SDL_BUTTON_LEFT:
      buttons_.left = down;
      break;
    case SDL_BUTTON_RIGHT:
      buttons_.right = down;
      break;
    case SDL_BUTTON_MIDDLE:
      buttons_.middle = down;
      break;
    default:
      break;
  }
}

void InputMapper::Escape() {
  if (mode_ == camera::CameraMode::kFly && mouse_captured_) {
    mouse_captured_ = false;
  } else {
    quit_requested_ = true;
  }
}

void InputMapper::ToggleMode() {
  if (mode_ == camera::CameraMode::kFly) {
    mode_ = camera::CameraMode::kOrbit;
    mouse_captured_ = false;
  } else {
    mode_ = camera::CameraMode::kFly;
  }
  motion_ = glm::vec2(0.0f);
}

void InputMapper::CaptureMouse() {
  if (!mouse_captured_) {
    mouse_captured_ = true;
    discard_next_motion_ = true;
  }
}

void InputMapper::ReleaseInput() {
  mouse_captured_ = false;
  keys_ = {};
  buttons_ = {};
}

bool InputMapper::IsActivationClick(const SDL_Event& event) const {
  // The click that focuses the window only activates it. SDL's Windows backend
  // already swallows that click (unless SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH is
  // set), so this is the portable guarantee.
  return !focused_ ||
         event.button.timestamp - focus_gained_at_ < kActivationClickWindow;
}

bool InputMapper::LookActive() const {
  return mode_ == camera::CameraMode::kFly ? mouse_captured_ : buttons_.left;
}

bool InputMapper::PanActive() const {
  return mode_ == camera::CameraMode::kOrbit &&
         (buttons_.right || buttons_.middle);
}

}  // namespace blue::platform
