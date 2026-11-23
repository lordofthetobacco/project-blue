#ifndef BLUE_RENDER_VULKAN_PASSES_FRAME_CONTEXT_H_
#define BLUE_RENDER_VULKAN_PASSES_FRAME_CONTEXT_H_

#include <vulkan/vulkan.h>

#include <cstddef>
#include <span>

#include "camera/frustum.h"
#include "lighting/cascades.h"
#include "render/draw_list.h"
#include "render/frame_plan.h"
#include "render/scene_view.h"
#include "render/vulkan/image.h"
#include "render/vulkan/render_targets.h"

namespace blue::render::vulkan {

// What every pass gets when it records its part of one frame. Built by
// VulkanRenderer::RecordCommands() and only valid during that call;
// everything is borrowed from the renderer.
struct FrameContext {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  size_t frame_index = 0;  // Frame in flight, [0, kFramesInFlight).
  // FrameGlobals' set 0 of this frame: FrameUniforms and shared images.
  VkDescriptorSet frame_set = VK_NULL_HANDLE;
  const SceneView& view;
  // Which passes run this frame (the main view's plan, also for the scope
  // view).
  const FramePlan& plan;
  const RenderTargets& targets;
  // What the camera passes (DepthPrepass, the scene pass and what is drawn
  // inside it) render into, from the top-left corner: targets.hdr(),
  // targets.depth() and targets.extent() for the main view; a scope's
  // picture for the scope view (VulkanRenderer::RecordScopeView()).
  const Image& scene_hdr;
  const Image& scene_depth;
  VkExtent2D scene_extent{};

  // The swapchain image this frame presents (layout UNDEFINED until the
  // present pass).
  VkImage swapchain_image = VK_NULL_HANDLE;
  VkImageView swapchain_view = VK_NULL_HANDLE;
  VkExtent2D swapchain_extent{};

  // Culling inputs: world-space bounds of view.draws (parallel to it) and
  // the camera frustum.
  std::span<const camera::Aabb> world_bounds;
  const camera::Frustum& camera_frustum;
  // The sun's shadow cascades; only meaningful when `shadows` is true.
  std::span<const lighting::Cascade> cascades;
  bool shadows = false;
  // The visible submeshes, sorted (DrawList), for the camera passes.
  std::span<const DrawEntry> draw_list;

  RenderStats& stats;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_FRAME_CONTEXT_H_
