#include "platform/Window.hpp"

#include "core/Log.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

namespace blue {

Window::Window(const char* title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log::fatal("SDL_Init failed: {}", SDL_GetError());
    }

    constexpr SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(title, width, height, flags);
    if (window_ == nullptr) {
        log::fatal("SDL_CreateWindow failed: {}", SDL_GetError());
    }
}

Window::~Window()
{
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

VkExtent2D Window::pixelSize() const
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

float Window::displayScale() const
{
    const float scale = SDL_GetWindowDisplayScale(window_);
    return scale > 0.0f ? scale : 1.0f;
}

bool Window::isMinimized() const
{
    return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

void Window::setRelativeMouseMode(bool enabled)
{
    if (!SDL_SetWindowRelativeMouseMode(window_, enabled)) {
        log::error("SDL_SetWindowRelativeMouseMode({}) failed: {}", enabled, SDL_GetError());
    }
}

std::span<const char* const> Window::requiredInstanceExtensions() const
{
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr) {
        log::fatal("SDL_Vulkan_GetInstanceExtensions failed: {}", SDL_GetError());
    }
    return {extensions, count};
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
        log::fatal("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
    }
    return surface;
}

} // namespace blue
