#pragma once

#include <vulkan/vulkan.h>

#include <span>

struct SDL_Window;

namespace blue {

// Owns SDL initialization and the native window. Vulkan-capable by construction.
class Window {
public:
    Window(const char* title, int width, int height);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] SDL_Window* handle() const { return window_; }

    // Size of the drawable area in pixels (may differ from window size on HiDPI).
    [[nodiscard]] VkExtent2D pixelSize() const;
    [[nodiscard]] float displayScale() const;
    [[nodiscard]] bool isMinimized() const;

    // Relative mode hides the cursor, confines it to the window and reports raw device motion.
    void setRelativeMouseMode(bool enabled);

    [[nodiscard]] std::span<const char* const> requiredInstanceExtensions() const;
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

private:
    SDL_Window* window_ = nullptr;
};

} // namespace blue
