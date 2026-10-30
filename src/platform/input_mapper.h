#ifndef BLUE_PLATFORM_INPUT_MAPPER_H_
#define BLUE_PLATFORM_INPUT_MAPPER_H_

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>

#include "camera/camera_controller.h"
#include "input/input_frame.h"

namespace blue::platform {

// Discrete, one-shot requests the app acts on; continuous input goes through
// input::InputFrame.
enum class InputAction {
  kToggleInvertY,
  kSensitivityDown,
  kSensitivityUp,
  kToggleOverlay,
  kSaveScene,
  kLoadScene,
  kToggleVsync,
  kPush,  // Push the object under the crosshair.
};

// Interprets SDL events as camera input. Pure logic over event data: it never
// calls into SDL, so it works without a window. Side effects (capturing the
// cursor, quitting) are exposed as requests. Copyable. Not thread-safe.
class InputMapper {
 public:
  InputMapper() = default;
  InputMapper(const InputMapper&) = default;
  InputMapper& operator=(const InputMapper&) = default;
  InputMapper(InputMapper&&) = default;
  InputMapper& operator=(InputMapper&&) = default;

  // Feeds one SDL event into the mapper. Events other than window state are
  // ignored while the window is minimized.
  void HandleEvent(const SDL_Event& event);

  // Returns the input accumulated since the previous call and resets the
  // per-frame sums.
  [[nodiscard]] input::InputFrame EndFrame();

  // Moves out the actions triggered since the previous call.
  [[nodiscard]] std::vector<InputAction> TakeActions();

  [[nodiscard]] camera::CameraMode mode() const { return mode_; }
  [[nodiscard]] bool wants_mouse_captured() const { return mouse_captured_; }
  [[nodiscard]] bool quit_requested() const { return quit_requested_; }

 private:
  struct HeldKeys {
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    bool sprint = false;
  };

  struct HeldButtons {
    bool left = false;
    bool right = false;
    bool middle = false;
  };

  void HandleKey(const SDL_Event& event);
  void HandleMouseButton(const SDL_Event& event);
  void HandleMouseMotion(const SDL_Event& event);
  void SetHeldKey(int scancode, bool down);
  void SetHeldButton(int button, bool down);
  void Escape();
  void ToggleMode();
  void CaptureMouse();
  void ReleaseInput();
  [[nodiscard]] bool IsActivationClick(const SDL_Event& event) const;
  [[nodiscard]] bool LookActive() const;
  [[nodiscard]] bool PanActive() const;

  camera::CameraMode mode_ = camera::CameraMode::kFly;
  HeldKeys keys_;
  HeldButtons buttons_;
  glm::vec2 motion_{0.0f};
  float wheel_ = 0.0f;
  std::vector<InputAction> actions_;

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
