#include "platform/input_mapper.h"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace blue::platform {

namespace {

SDL_Event KeyEvent(SDL_Scancode scancode, SDL_Keycode key, bool down,
                   bool repeat = false) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  event.key.scancode = scancode;
  event.key.key = key;
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

void Press(InputMapper& mapper, SDL_Scancode scancode, SDL_Keycode key) {
  mapper.HandleEvent(KeyEvent(scancode, key, true));
  mapper.HandleEvent(KeyEvent(scancode, key, false));
}

// Clicks in Fly mode and swallows the discarded first motion so tests start
// from a steady look state.
void CaptureAndSettle(InputMapper& mapper) {
  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, false));
  mapper.HandleEvent(MotionEvent(0.0f, 0.0f));
  (void)mapper.EndFrame();
}

bool Contains(const std::vector<InputAction>& actions, InputAction action) {
  return std::ranges::find(actions, action) != actions.end();
}

}  // namespace

TEST_CASE("InputMapper starts in Fly mode with nothing requested") {
  InputMapper mapper;
  CHECK(mapper.mode() == camera::CameraMode::kFly);
  CHECK_FALSE(mapper.wants_mouse_captured());
  CHECK_FALSE(mapper.quit_requested());
  CHECK(mapper.TakeActions().empty());
}

TEST_CASE("Mouse motion is ignored in Fly mode until the mouse is captured") {
  InputMapper mapper;
  mapper.HandleEvent(MotionEvent(10.0f, 5.0f));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.mouse_delta == glm::vec2(0.0f));
  CHECK_FALSE(frame.look);
}

TEST_CASE(
    "Left click captures the mouse in Fly mode and drops the first motion") {
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

TEST_CASE("Splitting motion across frames preserves the total") {
  InputMapper mapper;
  CaptureAndSettle(mapper);

  glm::vec2 total(0.0f);
  for (int i = 0; i < 100; ++i) {
    mapper.HandleEvent(MotionEvent(1.0f, -0.5f));
    total += mapper.EndFrame().mouse_delta;
  }
  CHECK(total.x == doctest::Approx(100.0f));
  CHECK(total.y == doctest::Approx(-50.0f));
}

TEST_CASE("Held movement keys come from scancodes and combine into axes") {
  InputMapper mapper;
  mapper.HandleEvent(
      KeyEvent(SDL_SCANCODE_W, SDLK_Z,
               true));  // layout-independent: the keycode is irrelevant
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, SDLK_D, true));
  input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move ==
        glm::vec3(1.0f, 0.0f, 1.0f));  // normalization is the camera's job

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_Z, false));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_S, SDLK_S, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, SDLK_SPACE, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, true));
  frame = mapper.EndFrame();
  CHECK(frame.move == glm::vec3(1.0f, 1.0f, -1.0f));
  CHECK(frame.sprint);

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_SPACE, SDLK_SPACE, false));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_LCTRL, SDLK_LCTRL, true));
  CHECK(mapper.EndFrame().move.y == -1.0f);
}

TEST_CASE("Held state persists across frames and opposite keys cancel") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_A, SDLK_A, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, SDLK_D, true));
  CHECK(mapper.EndFrame().move.x == 0.0f);
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_D, SDLK_D, false));
  CHECK(mapper.EndFrame().move.x == -1.0f);
  CHECK(mapper.EndFrame().move.x == -1.0f);
}

TEST_CASE("Discrete actions fire once per press and ignore key repeat") {
  InputMapper mapper;
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, SDLK_I, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, SDLK_I, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, SDLK_I, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, SDLK_I, false));

  const std::vector<InputAction> actions = mapper.TakeActions();
  REQUIRE(actions.size() == 1);
  CHECK(actions.front() == InputAction::kToggleInvertY);
  CHECK(mapper.TakeActions().empty());
}

TEST_CASE("Every bound key maps to its action") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET);
  Press(mapper, SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET);
  Press(mapper, SDL_SCANCODE_F1, SDLK_F1);
  Press(mapper, SDL_SCANCODE_F5, SDLK_F5);
  Press(mapper, SDL_SCANCODE_F9, SDLK_F9);
  Press(mapper, SDL_SCANCODE_V, SDLK_V);

  const std::vector<InputAction> actions = mapper.TakeActions();
  REQUIRE(actions.size() == 6);
  CHECK(Contains(actions, InputAction::kSensitivityDown));
  CHECK(Contains(actions, InputAction::kSensitivityUp));
  CHECK(Contains(actions, InputAction::kToggleOverlay));
  CHECK(Contains(actions, InputAction::kSaveScene));
  CHECK(Contains(actions, InputAction::kLoadScene));
  CHECK(Contains(actions, InputAction::kToggleVsync));
}

TEST_CASE("F requests a push once, ignores key repeat, in both modes") {
  InputMapper mapper;

  SUBCASE("Fly") {}
  SUBCASE("Orbit") { Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB); }

  (void)mapper.TakeActions();
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_F, SDLK_F, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_F, SDLK_F, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_F, SDLK_F, true, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_F, SDLK_F, false));

  const std::vector<InputAction> actions = mapper.TakeActions();
  REQUIRE(actions.size() == 1);
  CHECK(actions.front() == InputAction::kPush);
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

  Press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
  CHECK_FALSE(mapper.wants_mouse_captured());
  CHECK_FALSE(mapper.quit_requested());

  Press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
  CHECK(mapper.quit_requested());
}

TEST_CASE("Escape quits immediately when the mouse is not captured") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
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
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_W, true));
  REQUIRE(mapper.EndFrame().move.z == 1.0f);

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
  CHECK_FALSE(mapper.wants_mouse_captured());
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.move == glm::vec3(0.0f));
  CHECK_FALSE(frame.look);

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
  CHECK_FALSE(mapper.wants_mouse_captured());
  mapper.HandleEvent(MotionEvent(5.0f, 5.0f));
  CHECK(mapper.EndFrame().mouse_delta == glm::vec2(0.0f));
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

  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_W, true));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_I, SDLK_I, true));
  mapper.HandleEvent(WheelEvent(2.0f));
  mapper.HandleEvent(MotionEvent(9.0f, 9.0f));
  const input::InputFrame frame = mapper.EndFrame();
  CHECK(frame.mouse_delta == glm::vec2(0.0f));
  CHECK(frame.wheel == 0.0f);
  CHECK(frame.move == glm::vec3(0.0f));
  CHECK_FALSE(frame.look);
  CHECK(mapper.TakeActions().empty());
  CHECK_FALSE(mapper.wants_mouse_captured());

  mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_RESTORED));
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_W, true));
  CHECK(mapper.EndFrame().move.z == 1.0f);
}

TEST_CASE("Maximize and focus gain also end the minimized state") {
  for (const SDL_EventType type :
       {SDL_EVENT_WINDOW_MAXIMIZED, SDL_EVENT_WINDOW_FOCUS_GAINED}) {
    InputMapper mapper;
    mapper.HandleEvent(WindowEvent(SDL_EVENT_WINDOW_MINIMIZED));
    mapper.HandleEvent(WindowEvent(type));
    mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_W, true));
    CHECK(mapper.EndFrame().move.z == 1.0f);
  }
}

TEST_CASE("Tab switches to Orbit, releases the capture and switches back") {
  InputMapper mapper;
  CaptureAndSettle(mapper);
  REQUIRE(mapper.wants_mouse_captured());

  Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
  CHECK(mapper.mode() == camera::CameraMode::kOrbit);
  CHECK_FALSE(mapper.wants_mouse_captured());

  Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
  CHECK(mapper.mode() == camera::CameraMode::kFly);
  CHECK_FALSE(mapper.wants_mouse_captured());
}

TEST_CASE(
    "Orbit: left drag rotates, right and middle drag pan, cursor is never "
    "captured") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);

  mapper.HandleEvent(MotionEvent(7.0f, 7.0f));  // no button held
  CHECK(mapper.EndFrame().mouse_delta == glm::vec2(0.0f));

  mapper.HandleEvent(ButtonEvent(SDL_BUTTON_LEFT, true));
  CHECK_FALSE(mapper.wants_mouse_captured());
  mapper.HandleEvent(
      MotionEvent(3.0f, 1.0f));  // no first-motion discard in Orbit
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

TEST_CASE("Orbit: Escape quits and movement keys are not reported") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
  mapper.HandleEvent(KeyEvent(SDL_SCANCODE_W, SDLK_W, true));
  CHECK(mapper.EndFrame().move == glm::vec3(0.0f));

  Press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
  CHECK(mapper.quit_requested());
}

TEST_CASE("Orbit: focus loss clears held mouse buttons") {
  InputMapper mapper;
  Press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
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

}  // namespace blue::platform
