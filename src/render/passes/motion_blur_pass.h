#ifndef BLUE_RENDER_PASSES_MOTION_BLUR_PASS_H_
#define BLUE_RENDER_PASSES_MOTION_BLUR_PASS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "render/fullscreen_pass.h"
#include "render/mesh.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Per-object motion blur of the draw items that have a previous_transform
// (the first-person weapon), on the linear HDR image before bloom. The world
// is never blurred: an item's velocity is its motion relative to the camera,
// so a weapon rigidly following the view stays sharp however fast the
// player turns. Blur lengths and their frame-rate independence are defined
// in render/motion_blur_math.h. Four steps, all restricted to `area`
// (FindArea(): the screen rectangle of the moving items' bounds, clipped to
// the near plane, grown by the largest blur radius), since nothing outside
// it can change:
//
//   1. velocity   the moving items' entries of the draw list -> velocity
//                 (shaders/motion_velocity.*): blur vector and coverage,
//                 depth tested EQUAL against the pre-pass like the mesh
//                 pass, so only their visible surface writes.
//   2. tile max   velocity -> motion_tiles (shaders/motion_tile_max.frag):
//                 the longest blur vector per tile of the largest blur
//                 radius.
//   3. copy       hdr -> motion_source, the area only (the blur reads the
//                 image it writes).
//   4. blur       motion_source + velocity + motion_tiles -> hdr
//                 (shaders/motion_blur.frag): a gather along the longest
//                 blur vector of the 3 x 3 tiles around each pixel. Only the
//                 moving items' colour spreads, softly past their
//                 silhouette; a pixel no moving item reaches is copied back
//                 unchanged, bit for bit.
//
// Costs nothing in frames without such an item or with the blur off (the
// caller skips it, see FindArea()). Draws nothing over the reflex sight:
// the renderer draws the sight after this pass.
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class MotionBlurPass {
 public:
  MotionBlurPass(const VulkanContext& context, ShaderPrograms& programs,
                 const std::string& shader_dir,
                 VkDescriptorSetLayout frame_layout,
                 const std::vector<Mesh>& meshes);

  MotionBlurPass(const MotionBlurPass&) = delete;
  MotionBlurPass& operator=(const MotionBlurPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized, while the GPU is idle.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Returns the pixels the blur may change this frame, or nullopt when there
  // is nothing to do: the blur is off or its scale is 0 (strength, frame
  // duration), or no visible draw-list entry has a previous_transform.
  [[nodiscard]] static std::optional<VkRect2D> FindArea(
      const FrameContext& ctx);

  // Records the four steps over `area` (from FindArea()). Expects the HDR
  // target in SHADER_READ_ONLY_OPTIMAL after the scene pass and the depth
  // target in DEPTH_READ_ONLY_OPTIMAL; leaves the HDR target in
  // SHADER_READ_ONLY_OPTIMAL (last written as a color attachment), the
  // velocity, tile and source images in SHADER_READ_ONLY_OPTIMAL. Must be
  // recorded outside of a rendering pass.
  void Record(const FrameContext& ctx, const VkRect2D& area) const;

 private:
  // Step 1: clears the velocity target and draws the moving items into it.
  void RecordVelocity(const FrameContext& ctx) const;
  // Step 2: the tile max of the velocity in `area`.
  void RecordTileMax(const FrameContext& ctx, const VkRect2D& area) const;
  // Step 3: copies `area` of the HDR target into motion_source and leaves
  // the HDR target ready to be drawn into (COLOR_ATTACHMENT_OPTIMAL).
  void RecordSourceCopy(const FrameContext& ctx, const VkRect2D& area) const;

  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  const std::vector<Mesh>& meshes_;
  // Velocity pipelines, [0] culling back faces, [1] double-sided; replaced
  // when the shaders are reloaded, never null after construction.
  std::array<std::unique_ptr<Pipeline>, 2> velocity_pipelines_;
  FullscreenPass tile_max_;
  FullscreenPass blur_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_MOTION_BLUR_PASS_H_
