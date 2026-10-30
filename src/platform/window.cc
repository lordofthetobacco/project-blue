#include "platform/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "core/log.h"

namespace blue::platform {

Window::Window(const char* title, int width, int height) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    core::log::Fatal("SDL_Init failed: {}", SDL_GetError());
  }

  constexpr SDL_WindowFlags kFlags =
      SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  window_ = SDL_CreateWindow(title, width, height, kFlags);
  if (window_ == nullptr) {
    core::log::Fatal("SDL_CreateWindow failed: {}", SDL_GetError());
  }
}

Window::~Window() {
  SDL_DestroyWindow(window_);
  SDL_Quit();
}

VkExtent2D Window::PixelSize() const {
  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window_, &width, &height);
  return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

float Window::DisplayScale() const {
  const float scale = SDL_GetWindowDisplayScale(window_);
  return scale > 0.0f ? scale : 1.0f;
}

int Window::width() const {
  int width = 0;
  SDL_GetWindowSize(window_, &width, nullptr);
  return width;
}

int Window::height() const {
  int height = 0;
  SDL_GetWindowSize(window_, nullptr, &height);
  return height;
}

bool Window::IsMinimized() const {
  return (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
}

void Window::SetRelativeMouseMode(bool enabled) {
  if (!SDL_SetWindowRelativeMouseMode(window_, enabled)) {
    core::log::Error("SDL_SetWindowRelativeMouseMode({}) failed: {}", enabled,
                     SDL_GetError());
  }
}

std::span<const char* const> Window::RequiredInstanceExtensions() const {
  Uint32 count = 0;
  const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
  if (extensions == nullptr) {
    core::log::Fatal("SDL_Vulkan_GetInstanceExtensions failed: {}",
                     SDL_GetError());
  }
  return {extensions, count};
}

VkSurfaceKHR Window::CreateSurface(VkInstance instance) const {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
    core::log::Fatal("SDL_Vulkan_CreateSurface failed: {}", SDL_GetError());
  }
  return surface;
}

}  // namespace blue::platform
