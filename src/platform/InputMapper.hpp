#pragma once

#include "camera/CameraController.hpp"
#include "input/InputFrame.hpp"

#include <cstdint>
#include <vector>

union SDL_Event;

namespace blue {

// Discrete, one-shot requests the app acts on; continuous input goes through InputFrame.
enum class InputAction {
    ToggleInvertY,
    SensitivityDown,
    SensitivityUp,
    ToggleOverlay,
    SaveScene,
    LoadScene,
    ToggleVsync,
};

// Interprets SDL events as camera input. Pure logic over event data: it never calls into SDL, so
// it works without a window. Side effects (capturing the cursor, quitting) are exposed as requests.
class InputMapper {
public:
    void handleEvent(const SDL_Event& event);

    // Returns the input accumulated since the previous call and resets the per-frame sums.
    [[nodiscard]] InputFrame endFrame();

    // Moves out the actions triggered since the previous call.
    [[nodiscard]] std::vector<InputAction> takeActions();

    [[nodiscard]] CameraMode mode() const { return mode_; }
    [[nodiscard]] bool wantsMouseCaptured() const { return mouseCaptured_; }
    [[nodiscard]] bool quitRequested() const { return quitRequested_; }

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

    void handleKey(const SDL_Event& event);
    void handleMouseButton(const SDL_Event& event);
    void handleMouseMotion(const SDL_Event& event);
    void setHeldKey(int scancode, bool down);
    void setHeldButton(int button, bool down);
    void escape();
    void toggleMode();
    void captureMouse();
    void releaseInput();
    [[nodiscard]] bool isActivationClick(const SDL_Event& event) const;
    [[nodiscard]] bool lookActive() const;
    [[nodiscard]] bool panActive() const;

    CameraMode mode_ = CameraMode::Fly;
    HeldKeys keys_;
    HeldButtons buttons_;
    glm::vec2 motion_{0.0f};
    float wheel_ = 0.0f;
    std::vector<InputAction> actions_;

    std::uint64_t focusGainedAt_ = 0; // SDL event timestamp, nanoseconds.
    bool focused_ = false;            // Unfocused until the first FOCUS_GAINED.
    bool mouseCaptured_ = false;
    bool discardNextMotion_ = false; // SDL can report a jump when it warps the cursor on capture.
    bool minimized_ = false;
    bool quitRequested_ = false;
};

} // namespace blue
