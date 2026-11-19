#ifndef BLUE_RENDER_PASSES_OVERLAY_PASS_H_
#define BLUE_RENDER_PASSES_OVERLAY_PASS_H_

#include "render/metal_types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/fullscreen_pass.h"
#include "render/mesh.h"
#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/scene_view.h"
#include "render/shader_programs.h"

namespace blue::render {

// Editor overlays drawn over the tonemapped image (ctx.targets.ldr()), after
// the tonemap pass and before the present pass, so their colours are exact
// on screen instead of going through exposure, bloom and the tonemapper:
//
//   lines      SceneView::lines (depth-tested against the scene depth, not
//              writing it) and SceneView::overlay_lines (always on top), one
//              pixel wide, alpha-blended (shaders/line.{vert,frag}.metal).
//   selection  The items of SceneView::selected_draws are rendered into
//              ctx.targets.selection_mask() (R8: 0.5 where an item is
//              hidden by the scene, 1 where visible), then an edge filter
//              draws a 2-pixel outline in SceneView::selection_color around
//              the mask, dimmed where the item is hidden
//              (shaders/selection_mask.frag.metal,
//              shaders/selection_outline.frag.metal).
//
// Records nothing at all when the view has no lines and no selection (the
// game's case). Line vertices go through a per-frame-in-flight shared buffer
// that only grows, so steady use allocates nothing.
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class OverlayPass {
 public:
  OverlayPass(const MetalContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, const std::vector<Mesh>& meshes);

  OverlayPass(const OverlayPass&) = delete;
  OverlayPass& operator=(const OverlayPass&) = delete;

  // Points the outline filter at targets.selection_mask(). Call at startup
  // and whenever the render targets were recreated.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Returns true if `view` asks for any overlay.
  [[nodiscard]] static bool HasWork(const SceneView& view);

  // Records the overlays of ctx.view (see the class comment), each part in
  // its own render encoder; a no-op when HasWork() is false.
  void Record(const FrameContext& ctx);

 private:
  // Copies the line vertices of ctx.view into this frame's buffer, growing it
  // if needed.
  void UploadLines(const FrameContext& ctx);
  void RecordLines(const FrameContext& ctx) const;
  void RecordSelectionMask(const FrameContext& ctx) const;

  const MetalContext& context_;
  const std::vector<Mesh>& meshes_;
  // Per frame in flight: CPU-written line vertices.
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

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_OVERLAY_PASS_H_
