#include "platform/input_mapper.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <vector>

#include "input/action.h"
#include "input/bindings.h"

namespace blue::platform {

namespace {

using input::Action;
using input::Binding;

SDL_Event KeyEvent(SDL_Scancode scancode, bool down, bool repeat = false) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = scancode;
  event.key.down = down;
  event.key.repeat = repeat;
  return event;
}

constexpr Uint64 kMillisecond = 1'000'000;
constexpr Uint64 kSecond = 1'000 * kMillisecond;

SDL_Event ButtonEvent(Uint8 button, bool down, Uint64 timestamp = kSecond) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  event.button.button = button;
  event.button.down = down;
  event.common.timestamp = timestamp;
  return event;
}

SDL_Event MotionEvent(float xrel, float yrel) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_MOTION;
  event.motion.xrel = xrel;
  event.motion.yrel = yrel;
  return event;
}

SDL_Event WheelEvent(
    float y, SDL_MouseWheelDirection direction = SDL_MOUSEWHEEL_NORMAL) {
  SDL_Event event{};
  event.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.y = y;
  event.wheel.direction = direction;
  return event;
}

SDL_Event WindowEvent(SDL_EventType type, Uint64 timestamp = 0) {
  SDL_Event event{};
  event.type = type;
  event.common.timestamp = timestamp;
  return event;
}

void Press(InputMapper& mapper, SDL_Scancode scancode) {
  mapper.HandleEvent(KeyEvent(scancode, true));
  mapper.HandleEvent(KeyEvent(scancode, false));
}

// Clicks in capture mode and swallows the discarded first motion so tests
// start from a steady look state.
void CaptureAndSettle(InputMapper& mapper) {
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, false));
  mapper.HandleEvent(MotionEvent(0.0f, 0.0f));
  (void)mapper.EndFrame();
}

// Returns the number of actions reported as pressed in `frame`.
int PressedCount(const input::InputFrame& frame) {
  int count = 0;
  for (const Action action : input::AllActions()) {
    count += frame.actions.pressed(action) ? 1 : 0;
  }
  return count;
}

}  // namespace

TEST_CASE("InputMapper starts in capture mode with nothing requested") {
  InputMapper mapper;
  CHECK(mapper.mouse_mode() == InputMapper::MouseMode::kCapture);
  CHECK(mapper.bindings() == input::InputBindings::Defaults());
  CHECK_FALSE(mapper.wants_mouse_captured());
  CHECK_FALSE(mapper.quit_requested());
  CHECK(PressedCount(mapper.EndFrame()) == 0);
}

TEST_CASE(
    "Mouse motion is ignored in capture mode until the mouse is captured") {
  InputMapper mapper;
  mapper.HandleEvent(MotionEvent(10.0f, 5.0f));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.mouse_delta == glm::vec2(0.0f));
  CHECK_FALSE(frame.look);
}

TEST_CASE("Left click captures the mouse and drops the first motion") {
  InputMapper mapper;
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  CHECK(mapper.wants_mouse_captured());

  mapper.HandleEvent(MotionEvent(400.0f, -300.0f));  // warp jump
  mapper.HandleEvent(MotionEvent(3.0f, 2.0f));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.look);
  CHECK(frame.mouse_delta == glm::vec2(3.0f, 2.0f));
}

TEST_CASE("Motion is summed per frame with the SDL sign convention unchanged") {
  InputMapper mapper;
  CaptureAndSettle(mapper);

  mapper.HandleEvent(MotionEvent(4.0f, -6.0f));  // mouse right and up
  mapper.HandleEvent(MotionEvent(1.0f, -2.0f));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.mouse_delta.x == doctest::Approx(5.0f));
  CHECK(frame.mouse_delta.y == doctest::Approx(-8.0f));

  frame = mapper.EndFrame();
  CHECK(frame.mouse_delta == glm::vec2(0.0f));
}

TEST_CASE("Held movement keys come from scancodes and combine into axes") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, true));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move ==
        glm::vec3(1.0f, 0.0f, 1.0f));  // normalization is the camera's job
  CHECK(frame.actions.held(Action::kMoveForward));
  CHECK(frame.actions.pressed(Action::kMoveForward));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, false));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_S, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_LSHIFT, true));
  frame = mapper.EndFrame();
  CHECK(frame.move == glm::vec3(1.0f, 1.0f, -1.0f));
  CHECK(frame.sprint);
  CHECK(frame.actions.released(Action::kMoveForward));
  CHECK(frame.actions.held(Action::kJump));  // Space drives both actions
  CHECK(frame.actions.held(Action::kFlyUp));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, false));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_LCTRL, true));
  frame = mapper.EndFrame();
  CHECK(frame.move.y == -1.0f);
  CHECK(frame.actions.held(Action::kCrouch));
}

TEST_CASE("Held state persists across frames and opposite keys cancel") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_A, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, true));
  CHECK(mapper.EndFrame().move.x == 0.0f);
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, false));
  CHECK(mapper.EndFrame().move.x == -1.0f);
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move.x == -1.0f);
  CHECK_FALSE(frame.actions.pressed(Action::kMoveLeft));  // edges are per frame
  CHECK(frame.actions.held(Action::kMoveLeft));
}

TEST_CASE("Edge actions fire once per press and ignore key repeat") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, false));

  input::InputFrame frame = mapper.EndFrame();
  CHECK(PressedCount(frame) == 1);
  CHECK(frame.actions.pressed(Action::kToggleInvertY));
  CHECK(frame.actions.released(Action::kToggleInvertY));
  CHECK_FALSE(frame.actions.held(Action::kToggleInvertY));
  CHECK(PressedCount(mapper.EndFrame()) == 0);
}

TEST_CASE("Two keys bound to one action produce a single press edge") {
  input::InputBindings bindings;
  bindings.Set(Action::kJump, {Binding::Key(SDL_SCANCODE_SPACE),
                               Binding::Key(SDL_SCANCODE_KP_0)});
  InputMapper mapper(bindings);

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_KP_0, true));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.actions.pressed(Action::kJump));
  CHECK(frame.actions.held(Action::kJump));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, false));
  frame = mapper.EndFrame();
  CHECK(frame.actions.held(Action::kJump));  // still held by the keypad
  CHECK_FALSE(frame.actions.released(Action::kJump));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_KP_0, false));
  CHECK(mapper.EndFrame().actions.released(Action::kJump));
}

TEST_CASE("Every default binding maps to its action") {
  InputMapper mapper;
  CaptureAndSettle(mapper);  // Esc would otherwise quit; Mouse1 is swallowed
  const input::InputBindings& bindings = mapper.bindings();
  for (const Action action : input::AllActions()) {
    for (const Binding& binding : bindings.For(action)) {
      REQUIRE(binding.kind == input::BindingKind::kKey);
      Press(mapper, static_cast<SDL_Scancode>(binding.code));
      const input::InputFrame frame = mapper.EndFrame();
      CHECK_MESSAGE(frame.actions.pressed(action),
                    "action ", input::ActionName(action));
    }
  }
}

TEST_CASE("Custom bindings route a mouse button to jump once captured") {
  input::InputBindings bindings;
  bindings.Set(Action::kJump, {Binding::MouseButton(5)});
  InputMapper mapper;
  mapper.set_bindings(bindings);
  CaptureAndSettle(mapper);

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_X2, true));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.actions.pressed(Action::kJump));
  CHECK(frame.actions.held(Action::kJump));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_X2, false));
  CHECK(mapper.EndFrame().actions.released(Action::kJump));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, true));
  frame = mapper.EndFrame();
  CHECK_FALSE(frame.actions.held(Action::kJump));
  CHECK(frame.actions.held(Action::kFlyUp));
}

TEST_CASE("The capturing click is swallowed, later clicks feed Mouse1") {
  input::InputBindings bindings;
  bindings.Set(Action::kPush, {Binding::MouseButton(SDL_BUTTON_LEFT)});
  InputMapper mapper(bindings);
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, false));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(mapper.wants_mouse_captured());
  CHECK_FALSE(frame.actions.pressed(Action::kPush));
  CHECK_FALSE(frame.actions.released(Action::kPush));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  frame = mapper.EndFrame();
  CHECK(frame.actions.pressed(Action::kPush));
  CHECK(frame.actions.held(Action::kPush));
}

TEST_CASE("Wheel notches are summed and respect flipped direction") {
  InputMapper mapper;
  mapper.HandleEvent(WheelEvent(1.0f));
  mapper.HandleEvent(WheelEvent(2.0f));
  CHECK(mapper.EndFrame().wheel == doctest::Approx(3.0f));

  mapper.HandleEvent(WheelEvent(1.0f, SDL_MOUSEWHEEL_FLIPPED));
  CHECK(mapper.EndFrame().wheel == doctest::Approx(-1.0f));
  CHECK(mapper.EndFrame().wheel == 0.0f);
}

TEST_CASE("Escape releases the capture first and quits on the second press") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  REQUIRE(mapper.wants_mouse_captured());

  Press(mapper, SDL_SCANCODE_ESCAPE);
  CHECK_FALSE(mapper.wants_mouse_captured());
  CHECK_FALSE(mapper.quit_requested());
  CHECK(mapper.EndFrame().actions.pressed(Action::kEscape));

  Press(mapper, SDL_SCANCODE_ESCAPE);
  CHECK(mapper.quit_requested());
}

TEST_CASE("Escape quits immediately when the mouse is not captured") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_ESCAPE);
  CHECK(mapper.quit_requested());
}

TEST_CASE("A rebound escape key keeps the release-then-quit behaviour") {
  input::InputBindings bindings;
  bindings.Set(Action::kEscape, {Binding::Key(SDL_SCANCODE_Q)});
  InputMapper mapper(bindings);
  CaptureAndSettle(mapper);

  Press(mapper, SDL_SCANCODE_ESCAPE);
  CHECK(mapper.wants_mouse_captured());  // Esc is unbound now
  Press(mapper, SDL_SCANCODE_Q);
  CHECK_FALSE(mapper.wants_mouse_captured());
  CHECK_FALSE(mapper.quit_requested());
  Press(mapper, SDL_SCANCODE_Q);
  CHECK(mapper.quit_requested());
}

TEST_CASE("The window close event requests quit") {
  InputMapper mapper;
  mapper.HandleEvent(WindowEvent(SDL_EVENT_QUIT));
  CHECK(mapper.quit_requested());
}

TEST_CASE(
    "Focus loss releases the capture and held keys and gaining focus does not "
    "recapture") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
  REQUIRE(mapper.EndFrame().move.z == 1.0f);

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
  CHECK_FALSE(mapper.wants_mouse_captured());
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move == glm::vec3(0.0f));
  CHECK_FALSE(frame.look);
  CHECK_FALSE(frame.actions.held(Action::kMoveForward));
  CHECK_FALSE(frame.actions.released(Action::kMoveForward));  // no edge

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
  CHECK_FALSE(mapper.wants_mouse_captured());
  mapper.HandleEvent(MotionEvent(5.0f, 5.0f));
  CHECK(mapper.EndFrame().mouse_delta == glm::vec2(0.0f));

  // The physical key is still down; its release must not report an edge.
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, false));
  frame = mapper.EndFrame();
  CHECK_FALSE(frame.actions.released(Action::kMoveForward));
}

TEST_CASE("Recapturing after focus loss drops the first motion again") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, 5 * kSecond));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true, 6 * kSecond));
  mapper.HandleEvent(MotionEvent(300.0f, 300.0f));
  mapper.HandleEvent(MotionEvent(1.0f, 1.0f));
  CHECK(mapper.EndFrame().mouse_delta == glm::vec2(1.0f, 1.0f));
}

TEST_CASE("Input is ignored while minimized and frames are empty") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_MINIMIZED));

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, true));
  mapper.HandleEvent(WheelEvent(2.0f));
  mapper.HandleEvent(MotionEvent(9.0f, 9.0f));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.mouse_delta == glm::vec2(0.0f));
  CHECK(frame.wheel == 0.0f);
  CHECK(frame.move == glm::vec3(0.0f));
  CHECK_FALSE(frame.look);
  CHECK(PressedCount(frame) == 0);
  CHECK_FALSE(mapper.wants_mouse_captured());

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_RESTORED));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
  CHECK(mapper.EndFrame().move.z == 1.0f);
}

TEST_CASE("Maximize and focus gain also end the minimized state") {
  for (const SDL_EventType type :
       {SDL_EVENT_WINDOW_MAXIMIZED, SDL_EVENT_WINDOW_FOCUS_GAINED}) {
    InputMapper mapper;
    mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_MINIMIZED));
    mapper.HandleEvent(WindowEvent(type));
    mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
    CHECK(mapper.EndFrame().move.z == 1.0f);
  }
}

TEST_CASE("Tab is reported as the camera cycle action, not handled inside") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  Press(mapper, SDL_SCANCODE_TAB);
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.actions.pressed(Action::kCycleCamera));
  CHECK(mapper.mouse_mode() == InputMapper::MouseMode::kCapture);
  CHECK(mapper.wants_mouse_captured());
}

TEST_CASE("Switching to drag mode releases the capture and back keeps it off") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  REQUIRE(mapper.wants_mouse_captured());

  mapper.set_mouse_mode(InputMapper::MouseMode::kDrag);
  CHECK_FALSE(mapper.wants_mouse_captured());

  mapper.set_mouse_mode(InputMapper::MouseMode::kCapture);
  CHECK_FALSE(mapper.wants_mouse_captured());
}

TEST_CASE(
    "Drag mode: left drag rotates, right and middle drag pan, cursor is never "
    "captured") {
  InputMapper mapper;
  mapper.set_mouse_mode(InputMapper::MouseMode::kDrag);

  mapper.HandleEvent(MotionEvent(7.0f, 7.0f));  // no button held
  CHECK(mapper.EndFrame().mouse_delta == glm::vec2(0.0f));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  CHECK_FALSE(mapper.wants_mouse_captured());
  mapper.HandleEvent(
      MotionEvent(3.0f, 1.0f));  // no first-motion discard in drag mode
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.look);
  CHECK_FALSE(frame.pan);
  CHECK(frame.mouse_delta == glm::vec2(3.0f, 1.0f));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, false));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_RIGHT, true));
  mapper.HandleEvent(MotionEvent(2.0f, 0.0f));
  frame = mapper.EndFrame();
  CHECK(frame.pan);
  CHECK_FALSE(frame.look);
  CHECK(frame.mouse_delta == glm::vec2(2.0f, 0.0f));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_RIGHT, false));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_MIDDLE, true));
  CHECK(mapper.EndFrame().pan);
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_MIDDLE, false));
  CHECK_FALSE(mapper.EndFrame().pan);
}

TEST_CASE("Drag mode: Escape quits and movement axes are not reported") {
  InputMapper mapper;
  mapper.set_mouse_mode(InputMapper::MouseMode::kDrag);
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, true));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move == glm::vec3(0.0f));
  CHECK(frame.actions.held(Action::kMoveForward));  // still visible as action

  Press(mapper, SDL_SCANCODE_ESCAPE);
  CHECK(mapper.quit_requested());
}

TEST_CASE("Drag mode: focus loss clears held mouse buttons") {
  InputMapper mapper;
  mapper.set_mouse_mode(InputMapper::MouseMode::kDrag);
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
  CHECK_FALSE(mapper.EndFrame().look);
}

TEST_CASE("The click that activates the window does not capture the mouse") {
  InputMapper mapper;
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
  mapper.HandleEvent(
      ButtonEvent(SDL_BUTTON_LEFT, true, kSecond + 10 * kMillisecond));
  CHECK_FALSE(mapper.wants_mouse_captured());

  mapper.HandleEvent(
      ButtonEvent(SDL_BUTTON_LEFT, true, kSecond + 500 * kMillisecond));
  CHECK(mapper.wants_mouse_captured());
}

TEST_CASE(
    "A deliberate click after focus was gained without a click captures") {
  InputMapper mapper;
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true, 3 * kSecond));
  CHECK(mapper.wants_mouse_captured());
}

TEST_CASE("A click on a window that never gained focus does not capture") {
  InputMapper mapper;
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true, 10 * kSecond));
  CHECK_FALSE(mapper.wants_mouse_captured());
}

TEST_CASE("A click after focus loss does not capture until focus returns") {
  InputMapper mapper;
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, 2 * kSecond));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true, 3 * kSecond));
  CHECK_FALSE(mapper.wants_mouse_captured());
}

TEST_CASE("C and X toggle cascade debug and shadows once, ignoring repeat") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_C, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_C, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_C, false));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_X, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_X, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_X, false));

  const input::InputFrame frame = mapper.EndFrame();
  CHECK(PressedCount(frame) == 2);
  CHECK(frame.actions.pressed(Action::kToggleCascadeDebug));
  CHECK(frame.actions.pressed(Action::kToggleShadows));
}

}  // namespace blue::platform
