#ifndef BLUE_RENDER_PASSES_UI_PASS_H_
#define BLUE_RENDER_PASSES_UI_PASS_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "render/passes/frame_context.h"
#include "render/swapchain.h"
#include "render/texture_cache.h"
#include "render/texture_types.h"
#include "render/vulkan_context.h"

namespace blue::render {

// What the UI pass draws over.
enum class UiBackground {
  // The present pass's output: the swapchain image is in
  // COLOR_ATTACHMENT_OPTIMAL and kept.
  kScene,
  // Nothing (the editor, whose scene is in a panel): the freshly acquired
  // swapchain image is cleared to black first.
  kClear,
};

// Draws the ImGui overlay `ui` (when non-null) over the swapchain image
// without depth, then moves the image to PRESENT_SRC_KHR. The ImGui Vulkan
// backend must have been initialized (UiRenderer::Init()) when `ui` is
// non-null.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background);

// The ImGui Vulkan backend and the ImGui textures (descriptor sets of the
// backend's pool) the renderer hands out: the editor viewport image and
// uploaded textures. A replaced or released texture's set is freed only once
// no frame in flight can still use it (FreeRetired()). Starts uninitialized;
// every texture is 0 until Init(). Neither copyable nor movable. Not
// thread-safe.
class UiRenderer {
 public:
  UiRenderer() = default;

  UiRenderer(const UiRenderer&) = delete;
  UiRenderer& operator=(const UiRenderer&) = delete;

  // Initializes the backend to draw to `swapchain`'s format with dynamic
  // rendering and no depth (RecordUiPass()). Needs a current ImGui context.
  // Failure is fatal.
  void Init(const VulkanContext& context, const Swapchain& swapchain);
  // Shuts the backend down; its pool owns the texture sets, so this frees
  // them all. The GPU must be idle.
  void Shutdown();
  [[nodiscard]] bool initialized() const { return initialized_; }

  // Registers `view` (a sampled image in SHADER_READ_ONLY_OPTIMAL whenever
  // the UI samples it) as the viewport texture and returns it; the previous
  // viewport texture is retired. `frame_serial` is the last frame that may
  // have recorded the previous one. Returns 0 while uninitialized.
  [[nodiscard]] ImTextureID SetViewport(VkImageView view,
                                        uint64_t frame_serial);
  // Returns the texture of `id` in `textures`, registering it on first use;
  // unknown or released ids share one texture of the white default.
  // Returns 0 while uninitialized.
  [[nodiscard]] ImTextureID Texture(TextureId id,
                                    const TextureCache& textures);
  // Retires the texture of `id`, if any; `frame_serial` is the last frame
  // that may have recorded it.
  void Release(TextureId id, uint64_t frame_serial);
  // Frees the retired sets that no frame in flight can use any more, given
  // that `frame_serial` is the frame being recorded and every frame up to
  // frame_serial - kFramesInFlight has finished on the GPU.
  void FreeRetired(uint64_t frame_serial);

 private:
  // A texture descriptor set waiting until no frame in flight uses it.
  struct RetiredSet {
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t last_frame = 0;  // The last frame serial that may use it.
  };

  bool initialized_ = false;
  VkDescriptorSet viewport_set_ = VK_NULL_HANDLE;
  // The set of the white default, shared by every unknown or released id.
  VkDescriptorSet default_set_ = VK_NULL_HANDLE;
  // Sets of uploaded textures, indexed by TextureId; empty until first use.
  std::vector<VkDescriptorSet> texture_sets_;
  std::vector<RetiredSet> retired_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_UI_PASS_H_
