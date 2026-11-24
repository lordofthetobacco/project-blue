#ifndef BLUE_PLATFORM_WINDOW_H_
#define BLUE_PLATFORM_WINDOW_H_

#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_video.h>

#include <optional>
#include <string_view>

#include "core/extent.h"
#include "platform/window_graphics.h"

namespace blue::platform {

// Two integers given on the command line as "X,Y" (a window position in
// screen coordinates) or "W,H" (a window size in points).
struct WindowPair {
  int x = 0;
  int y = 0;
};

// Parses "<int>,<int>" (optional minus signs, no spaces), e.g. the value of
// --window-pos or --window-size. Returns nullopt for anything else.
[[nodiscard]] std::optional<WindowPair> ParseWindowPair(std::string_view text);

// Owns SDL initialization, the native window and, for
// WindowGraphics::kMetal, the Metal view (with its CAMetalLayer) the
// renderer draws into. Failure to initialize SDL or create the window or its
// view is fatal (Recreate() excepted). Neither copyable nor movable. Not
// thread-safe.
class Window {
 public:
  // Creates the window `width` x `height` points large, with its top-left
  // corner at `position` (screen coordinates) when given, else where the
  // system puts it. The window is placed before it is first shown, prepared
  // for `graphics`.
  Window(const char* title, int width, int height, WindowGraphics graphics,
         std::optional<WindowPair> position = std::nullopt);
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // Destroys the native window and creates it again, prepared for
  // `graphics`, with the same title, size and position (nothing to do when
  // it already is). Everything that borrowed handle(), native_handle() or
  // metal_layer() must have let go of them. Returns false, keeping a window
  // for the old graphics, when SDL cannot create one for `graphics` (e.g.
  // no Vulkan loader); failing to restore the old one is fatal.
  [[nodiscard]] bool Recreate(WindowGraphics graphics);

  [[nodiscard]] SDL_Window* handle() const { return window_; }
  [[nodiscard]] WindowGraphics graphics() const { return graphics_; }
  // Returns the native window handle as an untyped pointer (the HWND on
  // Windows; null elsewhere), so that this header includes no system header.
  [[nodiscard]] void* native_handle() const;
  // Returns the CAMetalLayer of the window's Metal view as an untyped
  // pointer, so that this header stays plain C++. The renderer bridges it
  // with `(__bridge CAMetalLayer*)`. Valid as long as the window lives; null
  // unless the window was created for WindowGraphics::kMetal.
  [[nodiscard]] void* metal_layer() const;

  // Returns the size of the drawable area in pixels (may differ from the
  // window size on HiDPI).
  [[nodiscard]] core::Extent2D PixelSize() const;
  // Returns the content scale of the display the window is on; always > 0.
  [[nodiscard]] float DisplayScale() const;
  // Logical (points, not pixels) window size; 0 if unknown.
  [[nodiscard]] int width() const;
  [[nodiscard]] int height() const;
  [[nodiscard]] bool IsMinimized() const;

  // Relative mode hides the cursor, confines it to the window and reports raw
  // device motion.
  void SetRelativeMouseMode(bool enabled);

 private:
  // Creates window_ (and, for kMetal, metal_view_) and sets graphics_;
  // false, with nothing created, when SDL fails (the error is logged).
  [[nodiscard]] bool Create(const char* title, int width, int height,
                            WindowGraphics graphics,
                            std::optional<WindowPair> position);
  // Destroys what Create() made.
  void Destroy();

  SDL_Window* window_ = nullptr;
  WindowGraphics graphics_;
  SDL_MetalView metal_view_ = nullptr;  // Only for WindowGraphics::kMetal.
};

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_WINDOW_H_
