#ifndef BLUE_RENDER_VULKAN_PASSES_OVERLAY_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_OVERLAY_PASS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/frame_config.h"
#include "render/vulkan/buffer.h"
#include "render/vulkan/fullscreen_pass.h"
#include "render/vulkan/mesh.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/render_targets.h"
#include "render/vulkan/samplers.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Editor overlays drawn over the tonemapped image (ctx.targets.ldr()), after
// the tonemap pass and before the present pass, so their colours are exact
// on screen instead of going through exposure, bloom and the tonemapper:
//
//   lines      SceneView::lines (depth-tested against the scene depth, not
//              writing it) and SceneView::overlay_lines (always on top), one
//              pixel wide, alpha-blended (shaders/line.slang).
//   selection  The items of SceneView::selected_draws are rendered into
//              ctx.targets.selection_mask() (R8: 0.5 where an item is
//              hidden by the scene, 1 where visible), then an edge filter
//              draws a 2-pixel outline in SceneView::selection_color around
//              the mask, dimmed where the item is hidden
//              (shaders/depth.slang, shaders/selection_outline.slang).
//
// Records nothing at all when the view has no lines and no selection (the
// game's case). Line vertices go through a per-frame-in-flight host-visible
// buffer that only grows, so steady use allocates nothing.
//
// Layouts: expects ldr in SHADER_READ_ONLY_OPTIMAL and depth in
// DEPTH_READ_ONLY_OPTIMAL and leaves them so; the selection mask is
// SHADER_READ_ONLY_OPTIMAL between frames.
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class OverlayPass {
 public:
  OverlayPass(const VulkanContext& context, ShaderPrograms& programs,
              VkDescriptorSetLayout frame_layout,
              const std::vector<Mesh>& meshes);

  OverlayPass(const OverlayPass&) = delete;
  OverlayPass& operator=(const OverlayPass&) = delete;

  // Points the outline filter at targets.selection_mask(). Call at startup
  // and whenever the render targets were recreated, with no frame in flight.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Records the overlays of ctx.view (see the class comment); a no-op when
  // FramePlan::overlay is false. Must be recorded outside of a rendering
  // scope.
  void Record(const FrameContext& ctx);

 private:
  // Copies the line vertices of ctx.view into this frame's buffer, growing it
  // if needed.
  void UploadLines(const FrameContext& ctx);
  void RecordLines(const FrameContext& ctx) const;
  void RecordSelectionMask(const FrameContext& ctx) const;

  const VulkanContext& context_;
  VkDescriptorSetLayout frame_layout_;
  const std::vector<Mesh>& meshes_;
  // Per frame in flight: host-visible line vertices.
  std::array<Buffer, kFramesInFlight> line_buffers_;
  // Replaced when the shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> tested_lines_;
  std::unique_ptr<Pipeline> top_lines_;
  // Selection mask: hidden parts (no depth test) then visible parts (depth
  // test against the scene).
  std::unique_ptr<Pipeline> mask_hidden_;
  std::unique_ptr<Pipeline> mask_visible_;
  FullscreenPass outline_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_OVERLAY_PASS_H_
