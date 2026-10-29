#include "platform/InputMapper.hpp"

#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <algorithm>

using namespace blue;

namespace {

SDL_Event keyEvent(SDL_Scancode scancode, SDL_Keycode key, bool down, bool repeat = false)
{
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

SDL_Event buttonEvent(Uint8 button, bool down, Uint64 timestamp = kSecond)
{
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.down = down;
    event.common.timestamp = timestamp;
    return event;
}

SDL_Event motionEvent(float xrel, float yrel)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.xrel = xrel;
    event.motion.yrel = yrel;
    return event;
}

SDL_Event wheelEvent(float y, SDL_MouseWheelDirection direction = SDL_MOUSEWHEEL_NORMAL)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = y;
    event.wheel.direction = direction;
    return event;
}

SDL_Event windowEvent(SDL_EventType type, Uint64 timestamp = 0)
{
    SDL_Event event{};
    event.type = type;
    event.common.timestamp = timestamp;
    return event;
}

void press(InputMapper& mapper, SDL_Scancode scancode, SDL_Keycode key)
{
    mapper.handleEvent(keyEvent(scancode, key, true));
    mapper.handleEvent(keyEvent(scancode, key, false));
}

// Clicks in Fly mode and swallows the discarded first motion so tests start from a steady look state.
void captureAndSettle(InputMapper& mapper)
{
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, false));
    mapper.handleEvent(motionEvent(0.0f, 0.0f));
    (void)mapper.endFrame();
}

bool contains(const std::vector<InputAction>& actions, InputAction action)
{
    return std::ranges::find(actions, action) != actions.end();
}

} // namespace

TEST_CASE("InputMapper starts in Fly mode with nothing requested")
{
    InputMapper mapper;
    CHECK(mapper.mode() == CameraMode::Fly);
    CHECK_FALSE(mapper.wantsMouseCaptured());
    CHECK_FALSE(mapper.quitRequested());
    CHECK(mapper.takeActions().empty());
}

TEST_CASE("Mouse motion is ignored in Fly mode until the mouse is captured")
{
    InputMapper mapper;
    mapper.handleEvent(motionEvent(10.0f, 5.0f));
    const InputFrame frame = mapper.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(0.0f));
    CHECK_FALSE(frame.look);
}

TEST_CASE("Left click captures the mouse in Fly mode and drops the first motion")
{
    InputMapper mapper;
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true));
    CHECK(mapper.wantsMouseCaptured());

    mapper.handleEvent(motionEvent(400.0f, -300.0f)); // warp jump
    mapper.handleEvent(motionEvent(3.0f, 2.0f));
    const InputFrame frame = mapper.endFrame();
    CHECK(frame.look);
    CHECK(frame.mouseDelta == glm::vec2(3.0f, 2.0f));
}

TEST_CASE("Motion is summed per frame with the SDL sign convention unchanged")
{
    InputMapper mapper;
    captureAndSettle(mapper);

    mapper.handleEvent(motionEvent(4.0f, -6.0f)); // mouse right and up
    mapper.handleEvent(motionEvent(1.0f, -2.0f));
    InputFrame frame = mapper.endFrame();
    CHECK(frame.mouseDelta.x == doctest::Approx(5.0f));
    CHECK(frame.mouseDelta.y == doctest::Approx(-8.0f));

    frame = mapper.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(0.0f));
}

TEST_CASE("Splitting motion across frames preserves the total")
{
    InputMapper mapper;
    captureAndSettle(mapper);

    glm::vec2 total(0.0f);
    for (int i = 0; i < 100; ++i) {
        mapper.handleEvent(motionEvent(1.0f, -0.5f));
        total += mapper.endFrame().mouseDelta;
    }
    CHECK(total.x == doctest::Approx(100.0f));
    CHECK(total.y == doctest::Approx(-50.0f));
}

TEST_CASE("Held movement keys come from scancodes and combine into axes")
{
    InputMapper mapper;
    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_Z, true)); // layout-independent: the keycode is irrelevant
    mapper.handleEvent(keyEvent(SDL_SCANCODE_D, SDLK_D, true));
    InputFrame frame = mapper.endFrame();
    CHECK(frame.move == glm::vec3(1.0f, 0.0f, 1.0f)); // normalization is the camera's job

    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_Z, false));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_S, SDLK_S, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_SPACE, SDLK_SPACE, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, true));
    frame = mapper.endFrame();
    CHECK(frame.move == glm::vec3(1.0f, 1.0f, -1.0f));
    CHECK(frame.sprint);

    mapper.handleEvent(keyEvent(SDL_SCANCODE_SPACE, SDLK_SPACE, false));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_LCTRL, SDLK_LCTRL, true));
    CHECK(mapper.endFrame().move.y == -1.0f);
}

TEST_CASE("Held state persists across frames and opposite keys cancel")
{
    InputMapper mapper;
    mapper.handleEvent(keyEvent(SDL_SCANCODE_A, SDLK_A, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_D, SDLK_D, true));
    CHECK(mapper.endFrame().move.x == 0.0f);
    mapper.handleEvent(keyEvent(SDL_SCANCODE_D, SDLK_D, false));
    CHECK(mapper.endFrame().move.x == -1.0f);
    CHECK(mapper.endFrame().move.x == -1.0f);
}

TEST_CASE("Discrete actions fire once per press and ignore key repeat")
{
    InputMapper mapper;
    mapper.handleEvent(keyEvent(SDL_SCANCODE_I, SDLK_I, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_I, SDLK_I, true, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_I, SDLK_I, true, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_I, SDLK_I, false));

    const std::vector<InputAction> actions = mapper.takeActions();
    REQUIRE(actions.size() == 1);
    CHECK(actions.front() == InputAction::ToggleInvertY);
    CHECK(mapper.takeActions().empty());
}

TEST_CASE("Every bound key maps to its action")
{
    InputMapper mapper;
    press(mapper, SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET);
    press(mapper, SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET);
    press(mapper, SDL_SCANCODE_F1, SDLK_F1);
    press(mapper, SDL_SCANCODE_F5, SDLK_F5);
    press(mapper, SDL_SCANCODE_F9, SDLK_F9);
    press(mapper, SDL_SCANCODE_V, SDLK_V);

    const std::vector<InputAction> actions = mapper.takeActions();
    REQUIRE(actions.size() == 6);
    CHECK(contains(actions, InputAction::SensitivityDown));
    CHECK(contains(actions, InputAction::SensitivityUp));
    CHECK(contains(actions, InputAction::ToggleOverlay));
    CHECK(contains(actions, InputAction::SaveScene));
    CHECK(contains(actions, InputAction::LoadScene));
    CHECK(contains(actions, InputAction::ToggleVsync));
}

TEST_CASE("Wheel notches are summed and respect flipped direction")
{
    InputMapper mapper;
    mapper.handleEvent(wheelEvent(1.0f));
    mapper.handleEvent(wheelEvent(2.0f));
    CHECK(mapper.endFrame().wheel == doctest::Approx(3.0f));

    mapper.handleEvent(wheelEvent(1.0f, SDL_MOUSEWHEEL_FLIPPED));
    CHECK(mapper.endFrame().wheel == doctest::Approx(-1.0f));
    CHECK(mapper.endFrame().wheel == 0.0f);
}

TEST_CASE("Escape releases the capture first and quits on the second press")
{
    InputMapper mapper;
    captureAndSettle(mapper);
    REQUIRE(mapper.wantsMouseCaptured());

    press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    CHECK_FALSE(mapper.wantsMouseCaptured());
    CHECK_FALSE(mapper.quitRequested());

    press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    CHECK(mapper.quitRequested());
}

TEST_CASE("Escape quits immediately when the mouse is not captured")
{
    InputMapper mapper;
    press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    CHECK(mapper.quitRequested());
}

TEST_CASE("The window close event requests quit")
{
    InputMapper mapper;
    mapper.handleEvent(windowEvent(SDL_EVENT_QUIT));
    CHECK(mapper.quitRequested());
}

TEST_CASE("Focus loss releases the capture and held keys and gaining focus does not recapture")
{
    InputMapper mapper;
    captureAndSettle(mapper);
    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_W, true));
    REQUIRE(mapper.endFrame().move.z == 1.0f);

    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
    CHECK_FALSE(mapper.wantsMouseCaptured());
    const InputFrame frame = mapper.endFrame();
    CHECK(frame.move == glm::vec3(0.0f));
    CHECK_FALSE(frame.look);

    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED));
    CHECK_FALSE(mapper.wantsMouseCaptured());
    mapper.handleEvent(motionEvent(5.0f, 5.0f));
    CHECK(mapper.endFrame().mouseDelta == glm::vec2(0.0f));
}

TEST_CASE("Recapturing after focus loss drops the first motion again")
{
    InputMapper mapper;
    captureAndSettle(mapper);
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));

    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, 5 * kSecond));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, 6 * kSecond));
    mapper.handleEvent(motionEvent(300.0f, 300.0f));
    mapper.handleEvent(motionEvent(1.0f, 1.0f));
    CHECK(mapper.endFrame().mouseDelta == glm::vec2(1.0f, 1.0f));
}

TEST_CASE("Input is ignored while minimized and frames are empty")
{
    InputMapper mapper;
    captureAndSettle(mapper);
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_MINIMIZED));

    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_W, true));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_I, SDLK_I, true));
    mapper.handleEvent(wheelEvent(2.0f));
    mapper.handleEvent(motionEvent(9.0f, 9.0f));
    const InputFrame frame = mapper.endFrame();
    CHECK(frame.mouseDelta == glm::vec2(0.0f));
    CHECK(frame.wheel == 0.0f);
    CHECK(frame.move == glm::vec3(0.0f));
    CHECK_FALSE(frame.look);
    CHECK(mapper.takeActions().empty());
    CHECK_FALSE(mapper.wantsMouseCaptured());

    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_RESTORED));
    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_W, true));
    CHECK(mapper.endFrame().move.z == 1.0f);
}

TEST_CASE("Maximize and focus gain also end the minimized state")
{
    for (const SDL_EventType type : {SDL_EVENT_WINDOW_MAXIMIZED, SDL_EVENT_WINDOW_FOCUS_GAINED}) {
        InputMapper mapper;
        mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_MINIMIZED));
        mapper.handleEvent(windowEvent(type));
        mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_W, true));
        CHECK(mapper.endFrame().move.z == 1.0f);
    }
}

TEST_CASE("Tab switches to Orbit, releases the capture and switches back")
{
    InputMapper mapper;
    captureAndSettle(mapper);
    REQUIRE(mapper.wantsMouseCaptured());

    press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
    CHECK(mapper.mode() == CameraMode::Orbit);
    CHECK_FALSE(mapper.wantsMouseCaptured());

    press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
    CHECK(mapper.mode() == CameraMode::Fly);
    CHECK_FALSE(mapper.wantsMouseCaptured());
}

TEST_CASE("Orbit: left drag rotates, right and middle drag pan, cursor is never captured")
{
    InputMapper mapper;
    press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);

    mapper.handleEvent(motionEvent(7.0f, 7.0f)); // no button held
    CHECK(mapper.endFrame().mouseDelta == glm::vec2(0.0f));

    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true));
    CHECK_FALSE(mapper.wantsMouseCaptured());
    mapper.handleEvent(motionEvent(3.0f, 1.0f)); // no first-motion discard in Orbit
    InputFrame frame = mapper.endFrame();
    CHECK(frame.look);
    CHECK_FALSE(frame.pan);
    CHECK(frame.mouseDelta == glm::vec2(3.0f, 1.0f));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, false));

    mapper.handleEvent(buttonEvent(SDL_BUTTON_RIGHT, true));
    mapper.handleEvent(motionEvent(2.0f, 0.0f));
    frame = mapper.endFrame();
    CHECK(frame.pan);
    CHECK_FALSE(frame.look);
    CHECK(frame.mouseDelta == glm::vec2(2.0f, 0.0f));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_RIGHT, false));

    mapper.handleEvent(buttonEvent(SDL_BUTTON_MIDDLE, true));
    CHECK(mapper.endFrame().pan);
    mapper.handleEvent(buttonEvent(SDL_BUTTON_MIDDLE, false));
    CHECK_FALSE(mapper.endFrame().pan);
}

TEST_CASE("Orbit: Escape quits and movement keys are not reported")
{
    InputMapper mapper;
    press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
    mapper.handleEvent(keyEvent(SDL_SCANCODE_W, SDLK_W, true));
    CHECK(mapper.endFrame().move == glm::vec3(0.0f));

    press(mapper, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE);
    CHECK(mapper.quitRequested());
}

TEST_CASE("Orbit: focus loss clears held mouse buttons")
{
    InputMapper mapper;
    press(mapper, SDL_SCANCODE_TAB, SDLK_TAB);
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true));
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST));
    CHECK_FALSE(mapper.endFrame().look);
}

TEST_CASE("The click that activates the window does not capture the mouse")
{
    InputMapper mapper;
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, kSecond + 10 * kMillisecond));
    CHECK_FALSE(mapper.wantsMouseCaptured());

    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, kSecond + 500 * kMillisecond));
    CHECK(mapper.wantsMouseCaptured());
}

TEST_CASE("A deliberate click after focus was gained without a click captures")
{
    InputMapper mapper;
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, 3 * kSecond));
    CHECK(mapper.wantsMouseCaptured());
}

TEST_CASE("A click on a window that never gained focus does not capture")
{
    InputMapper mapper;
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, 10 * kSecond));
    CHECK_FALSE(mapper.wantsMouseCaptured());
}

TEST_CASE("A click after focus loss does not capture until focus returns")
{
    InputMapper mapper;
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED, kSecond));
    mapper.handleEvent(windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST, 2 * kSecond));
    mapper.handleEvent(buttonEvent(SDL_BUTTON_LEFT, true, 3 * kSecond));
    CHECK_FALSE(mapper.wantsMouseCaptured());
}
