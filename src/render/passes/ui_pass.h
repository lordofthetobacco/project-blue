#ifndef BLUE_RENDER_PASSES_UI_PASS_H_
#define BLUE_RENDER_PASSES_UI_PASS_H_

#include "render/metal_types.h"

#include <imgui.h>

#include <cstdint>

#include "render/metal_context.h"
#include "render/passes/frame_context.h"

namespace blue::render {

// What the UI pass draws over.
enum class UiBackground {
  // What earlier passes of the frame rendered to the drawable (the present
  // pass's output): kept.
  kScene,
  // Nothing: the drawable is cleared first (to RecordUiPass()'s
  // `clear_color`).
  kClear,
};

// Opaque black, what the editor's window shows around its panels.
inline constexpr MTLClearColor kUiClearBlack{0.0, 0.0, 0.0, 1.0};

// Draws the ImGui overlay `ui` (when non-null) over the drawable of `ctx`,
// which must not be nil, in one render pass without depth. `clear_color` is
// linear (the drawable is sRGB) and only used with UiBackground::kClear. The
// ImGui Metal backend must have been initialized (UiRenderer::Init()) when
// `ui` is non-null. A frame whose UI was laid out for a larger output than
// the drawable (a resize between building the UI and acquiring the
// drawable) draws no UI.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background,
                  MTLClearColor clear_color = kUiClearBlack);

// Returns `texture` as an ImGui texture. The id does not keep the texture
// alive: whoever hands it to the UI keeps the texture alive for as long as a
// frame in flight may draw it.
[[nodiscard]] inline ImTextureID ToImTextureId(id<MTLTexture> texture) {
  return static_cast<ImTextureID>(
      reinterpret_cast<intptr_t>((__bridge void*)texture));
}

// The ImGui Metal backend. ImGui textures are bridged MTLTextures
// (ToImTextureId()), so the backend owns nothing but its own pipelines and
// font atlas. Starts uninitialized. Neither copyable nor movable. Not
// thread-safe.
class UiRenderer {
 public:
  UiRenderer() = default;

  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;

  // Initializes the backend on the device of `context`. Needs a current
  // ImGui context. Failure is fatal.
  void Init(const MetalContext& context);
  // Shuts the backend down, destroying its font atlas texture. The GPU must
  // be idle. Does nothing when uninitialized.
  void Shutdown();
  [[nodiscard]] bool initialized() const { return initialized_; }

 private:
  bool initialized_ = false;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_UI_PASS_H_
