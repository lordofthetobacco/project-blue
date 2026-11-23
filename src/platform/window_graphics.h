#ifndef BLUE_PLATFORM_WINDOW_GRAPHICS_H_
#define BLUE_PLATFORM_WINDOW_GRAPHICS_H_

#include <cstdint>

namespace blue::platform {

// The graphics API a platform::Window is created for, which decides how SDL
// prepares it (render::WindowGraphicsFor() maps a renderer backend to it).
enum class WindowGraphics : uint8_t {
  kNone,    // No API flag: the renderer uses the native handle (D3D12).
  kVulkan,  // SDL_WINDOW_VULKAN; the renderer creates its own surface.
  kMetal,   // SDL_WINDOW_METAL plus a Metal view with its CAMetalLayer.
};

}  // namespace blue::platform

#endif  // BLUE_PLATFORM_WINDOW_GRAPHICS_H_
