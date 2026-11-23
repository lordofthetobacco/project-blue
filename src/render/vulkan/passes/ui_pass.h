#ifndef BLUE_RENDER_VULKAN_PASSES_UI_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_UI_PASS_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

#include "render/texture_types.h"
#include "render/vulkan/barriers.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/texture_cache.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// What the UI pass draws over.
enum class UiBackground {
  // The present pass's output: the swapchain image is in
  // COLOR_ATTACHMENT_OPTIMAL and kept.
  kScene,
  // Nothing (the editor, whose scene is in a panel): the output image is
  // cleared to black first.
  kClear,
};

// How the output image is used around the UI pass.
struct UiOutputUse {
  // The last access before the pass, which a kClear transition waits for
  // (a freshly acquired swapchain image: nothing beyond the acquire
  // semaphore, waited for at the color attachment output stage).
  BarrierScope previous_use{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_NONE};
  // The layout the pass leaves the image in, and the next access, which
  // that transition makes wait (nothing for presenting: the submission's
  // semaphore orders it).
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  BarrierScope next_use;
};

// The window: the swapchain image is presented.
inline constexpr UiOutputUse kUiPresent{};
// Headless: the offscreen output image is read by copies (readbacks), also
// in the previous frame.
inline constexpr UiOutputUse kUiCopy{
    .previous_use = kCopySourceRead,
    .final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .next_use = kCopySourceRead,
};

// Draws the ImGui overlay `ui` (when non-null) over the output image
// (FrameContext::swapchain_image: the swapchain image, or the offscreen
// output image when headless) without depth, then moves the image as
// `use` says. The ImGui Vulkan backend must have been initialized for
// the output format and no depth attachment (UiRenderer::Init()) when `ui`
// is non-null.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background, const UiOutputUse& use);

// ImGui's Vulkan backend and the ImGui textures (descriptor sets of the
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

  // Initializes the backend to draw to images of `color_format` with dynamic
  // rendering and no depth (RecordUiPass()); `min_image_count` and
  // `image_count` describe the swapchain (at least 2 each). Needs a current
  // ImGui context. Failure is fatal.
  void Init(const VulkanContext& context, VkFormat color_format,
            uint32_t min_image_count, uint32_t image_count);
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

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_UI_PASS_H_
