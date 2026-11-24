#include "platform/window.h"

#include <SDL3/SDL.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
               WindowGraphics graphics, std::optional<WindowPair> position)
    : graphics_(graphics) {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    core::log::Fatal("SDL_Init failed: {}", SDL_GetError());
  }
  if (!Create(title, width, height, graphics, position)) {
    core::log::Fatal("The window could not be created (see above)");
  }
}

Window::~Window() {
  Destroy();
  SDL_Quit();
}

bool Window::Create(const char* title, int width, int height,
                    WindowGraphics graphics,
                    std::optional<WindowPair> position) {
  const SDL_PropertiesID properties = SDL_CreateProperties();
  SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                        height);
  for (const char* flag : {SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
                           SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN}) {
    SDL_SetBooleanProperty(properties, flag, true);
  }
  if (graphics == WindowGraphics::kVulkan) {
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN,
                           true);
  } else if (graphics == WindowGraphics::kMetal) {
    SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_METAL_BOOLEAN,
                           true);
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
    core::log::Error("SDL_CreateWindow failed: {}", SDL_GetError());
    return false;
  }
  graphics_ = graphics;
  if (graphics != WindowGraphics::kMetal) {
    return true;
  }
  metal_view_ = SDL_Metal_CreateView(window_);
  if (metal_view_ == nullptr) {
    core::log::Error("SDL_Metal_CreateView failed: {}", SDL_GetError());
    Destroy();
    return false;
  }
  return true;
}

void Window::Destroy() {
  // The view is a subview of the window's content view: it goes first.
  if (metal_view_ != nullptr) {
    SDL_Metal_DestroyView(metal_view_);
    metal_view_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

bool Window::Recreate(WindowGraphics graphics) {
  if (graphics == graphics_) {
    return true;
  }
  const std::string title = SDL_GetWindowTitle(window_);
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(window_, &width, &height);
  WindowPair position;
  SDL_GetWindowPosition(window_, &position.x, &position.y);
  const WindowGraphics previous = graphics_;
  Destroy();
  if (Create(title.c_str(), width, height, graphics, position)) {
    return true;
  }
  if (!Create(title.c_str(), width, height, previous, position)) {
    core::log::Fatal("The window could not be created again (see above)");
  }
  return false;
}

core::Extent2D Window::PixelSize() const {
  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window_, &width, &height);
  return {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
}

void* Window::native_handle() const {
  return SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

void* Window::metal_layer() const {
  return metal_view_ != nullptr ? SDL_Metal_GetLayer(metal_view_) : nullptr;
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

}  // namespace blue::platform
