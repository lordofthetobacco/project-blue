#include "platform/InputMapper.hpp"

#include <SDL3/SDL.h>

#include <utility>

namespace blue {

namespace {

constexpr std::uint64_t kActivationClickWindow = 200'000'000; // 200 ms in SDL timestamp nanoseconds

[[nodiscard]] float axis(bool positive, bool negative)
{
    return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

} // namespace

void InputMapper::handleEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_QUIT:
        quitRequested_ = true;
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        minimized_ = true;
        releaseInput();
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED: // Restoring a minimized maximized window may report only this.
        minimized_ = false;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        minimized_ = false; // A focused window is not minimized.
        focused_ = true;
        focusGainedAt_ = event.common.timestamp;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        // Never recapture on focus gain: the user may be alt-tabbing between fullscreen apps.
        focused_ = false;
        releaseInput();
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
        handleKey(event);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        handleMouseButton(event);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        handleMouseMotion(event);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        wheel_ += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        break;
    default:
        break;
    }
}

InputFrame InputMapper::endFrame()
{
    InputFrame frame;
    if (!minimized_) {
        frame.mouseDelta = motion_;
        frame.wheel = wheel_;
        if (mode_ == CameraMode::Fly) {
            frame.move = {axis(keys_.right, keys_.left), axis(keys_.up, keys_.down), axis(keys_.forward, keys_.back)};
            frame.sprint = keys_.sprint;
        }
        frame.look = lookActive();
        frame.pan = panActive();
    }
    motion_ = glm::vec2(0.0f);
    wheel_ = 0.0f;
    return frame;
}

std::vector<InputAction> InputMapper::takeActions()
{
    return std::exchange(actions_, {});
}

void InputMapper::handleKey(const SDL_Event& event)
{
    const bool down = event.type == SDL_EVENT_KEY_DOWN;
    setHeldKey(event.key.scancode, down);
    if (!down || event.key.repeat) {
        return;
    }

    switch (event.key.key) {
    case SDLK_ESCAPE: escape(); break;
    case SDLK_TAB: toggleMode(); break;
    case SDLK_I: actions_.push_back(InputAction::ToggleInvertY); break;
    case SDLK_LEFTBRACKET: actions_.push_back(InputAction::SensitivityDown); break;
    case SDLK_RIGHTBRACKET: actions_.push_back(InputAction::SensitivityUp); break;
    case SDLK_F1: actions_.push_back(InputAction::ToggleOverlay); break;
    case SDLK_F5: actions_.push_back(InputAction::SaveScene); break;
    case SDLK_F9: actions_.push_back(InputAction::LoadScene); break;
    case SDLK_V: actions_.push_back(InputAction::ToggleVsync); break;
    default: break;
    }
}

void InputMapper::handleMouseButton(const SDL_Event& event)
{
    const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    setHeldButton(event.button.button, down);
    if (down && event.button.button == SDL_BUTTON_LEFT && mode_ == CameraMode::Fly && !isActivationClick(event)) {
        captureMouse();
    }
}

void InputMapper::handleMouseMotion(const SDL_Event& event)
{
    if (!lookActive() && !panActive()) {
        return;
    }
    if (discardNextMotion_) {
        discardNextMotion_ = false;
        return;
    }
    // Raw counts, deliberately not scaled by frame time: the camera turns by delta * sensitivity.
    motion_ += glm::vec2(event.motion.xrel, event.motion.yrel);
}

void InputMapper::setHeldKey(int scancode, bool down)
{
    switch (scancode) {
    case SDL_SCANCODE_W: keys_.forward = down; break;
    case SDL_SCANCODE_S: keys_.back = down; break;
    case SDL_SCANCODE_A: keys_.left = down; break;
    case SDL_SCANCODE_D: keys_.right = down; break;
    case SDL_SCANCODE_SPACE: keys_.up = down; break;
    case SDL_SCANCODE_LCTRL: keys_.down = down; break;
    case SDL_SCANCODE_LSHIFT: keys_.sprint = down; break;
    default: break;
    }
}

void InputMapper::setHeldButton(int button, bool down)
{
    switch (button) {
    case SDL_BUTTON_LEFT: buttons_.left = down; break;
    case SDL_BUTTON_RIGHT: buttons_.right = down; break;
    case SDL_BUTTON_MIDDLE: buttons_.middle = down; break;
    default: break;
    }
}

void InputMapper::escape()
{
    if (mode_ == CameraMode::Fly && mouseCaptured_) {
        mouseCaptured_ = false;
    } else {
        quitRequested_ = true;
    }
}

void InputMapper::toggleMode()
{
    if (mode_ == CameraMode::Fly) {
        mode_ = CameraMode::Orbit;
        mouseCaptured_ = false;
    } else {
        mode_ = CameraMode::Fly;
    }
    motion_ = glm::vec2(0.0f);
}

void InputMapper::captureMouse()
{
    if (!mouseCaptured_) {
        mouseCaptured_ = true;
        discardNextMotion_ = true;
    }
}

void InputMapper::releaseInput()
{
    mouseCaptured_ = false;
    keys_ = {};
    buttons_ = {};
}

bool InputMapper::isActivationClick(const SDL_Event& event) const
{
    // The click that focuses the window only activates it. SDL's Windows backend already swallows that
    // click (unless SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH is set), so this is the portable guarantee.
    return !focused_ || event.button.timestamp - focusGainedAt_ < kActivationClickWindow;
}

bool InputMapper::lookActive() const
{
    return mode_ == CameraMode::Fly ? mouseCaptured_ : buttons_.left;
}

bool InputMapper::panActive() const
{
    return mode_ == CameraMode::Orbit && (buttons_.right || buttons_.middle);
}

} // namespace blue
