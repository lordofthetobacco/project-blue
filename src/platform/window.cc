#include "platform/window.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

#include "core/log.h"

namespace blue::platform {
namespace {

// Parses all of `text` as a decimal int.
bool ParseInt(std::string_view text, int& value) {
  const char* const end = text.data() + text.size();
  const std::from_chars_result result =
      std::from_chars(text.data(), end, value);
  return !text.empty() && result.ec == std::errc() && result.ptr == end;
}

}  // namespace

std::optional<WindowPair> ParseWindowPair(std::string_view text) {
  const size_t comma = text.find(',');
  WindowPair pair;
  if (comma == std::string_view::npos ||
      !ParseInt(text.substr(0, comma), pair.x) ||
      !ParseInt(text.substr(comma + 1), pair.y)) {
    return std::nullopt;
  }
  return pair;
}

Window::Window(const char* title, int width, int height,
               std::optional<WindowPair> position) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    core::log::Fatal("SDL_Init failed: {}", SDL_GetError());
  }

  const SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                        height);
  for (const char* flag : {SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN,
                           SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                           SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN}) {
    SDL_SetBooleanProperty(properties, flag, true);
  }
  if (position) {
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER,
                          position->x);
    SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER,
                          position->y);
  }
  window_ = SDL_CreateWindowWithProperties(properties);
  SDL_DestroyProperties(properties);
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
