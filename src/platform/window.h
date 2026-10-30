#ifndef BLUE_PLATFORM_WINDOW_H_
#define BLUE_PLATFORM_WINDOW_H_

#include <SDL3/SDL_video.h>
#include <vulkan/vulkan.h>

#include <span>

namespace blue::platform {

// Owns SDL initialization and the native window. Vulkan-capable by
// construction. Failure to initialize SDL or create the window is fatal.
// Neither copyable nor movable. Not thread-safe.
class Window {
 public:
  Window(const char* title, int width, int height);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  [[nodiscard]] SDL_Window* handle() const { return window_; }

  // Returns the size of the drawable area in pixels (may differ from the
  // window size on HiDPI).
  [[nodiscard]] VkExtent2D PixelSize() const;
  // Returns the content scale of the display the window is on; always > 0.
  [[nodiscard]] float DisplayScale() const;
  // Logical (points, not pixels) window size; 0 if unknown.
  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] bool IsMinimized() const;

  // Relative mode hides the cursor, confines it to the window and reports raw
  // device motion.
  void SetRelativeMouseMode(bool enabled);

  // Returns the instance extensions Vulkan needs to present to this window.
  // The array is owned by SDL and stays valid until SDL shuts down.
  [[nodiscard]] std::span<const char* const> RequiredInstanceExtensions() const;
  // Creates a presentation surface for `instance`. The caller owns it.
  [[nodiscard]] VkSurfaceKHR CreateSurface(VkInstance instance) const;

 private:
  SDL_Window* window_ = nullptr;
};

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_WINDOW_H_
