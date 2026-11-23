#ifndef BLUE_RENDER_VULKAN_PASSES_SHADOW_PASS_H_
#define BLUE_RENDER_VULKAN_PASSES_SHADOW_PASS_H_

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

#include "render/vulkan/image.h"
#include "render/vulkan/mesh.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Renders the sun's cascaded shadow map: one depth-only layer per cascade,
// every draw item whose world bounds reach the cascade. Alpha-masked
// materials cast solid shadows (the pass reads positions only).
//
// There is one map for all frames in flight. That is safe because the first
// barrier of Record() has the previous frame's fragment-shader sampled reads
// as its source scope: on the single queue it orders this pass's depth
// writes after those reads (and, transitively, after the previous writes).
//
// Borrows `context`, `programs` and `meshes`, which must outlive it.
// Neither copyable nor movable. Not thread-safe.
class ShadowPass {
 public:
  static constexpr VkFormat kFormat = VK_FORMAT_D16_UNORM;

  ShadowPass(const VulkanContext& context, ShaderPrograms& programs,
             const std::string& shader_dir, const std::vector<Mesh>& meshes);
  ~ShadowPass();

  ShadowPass(const ShadowPass&) = delete;
  ShadowPass& operator=(const ShadowPass&) = delete;

  // Renders the cascades when ctx.shadows is true; otherwise only makes sure
  // the map is readable. Must be recorded outside of a rendering pass,
  // before anything samples the map. Leaves the map in
  // DEPTH_READ_ONLY_OPTIMAL.
  void Record(const FrameContext& ctx);

  // The 2D-array view of all cascades and its comparison sampler (PCF).
  [[nodiscard]] VkImageView map_view() const { return map_.view(); }
  [[nodiscard]] VkSampler sampler() const { return sampler_; }
  // Device memory of the cascade map, in bytes.
  [[nodiscard]] VkDeviceSize map_bytes() const { return map_.size_bytes(); }

 private:
  // Moves the map out of its initial layout the first time, so it can be
  // bound while shadows are off.
  void InitializeLayout(VkCommandBuffer cmd);

  VkDevice device_;
  const std::vector<Mesh>& meshes_;
  Image map_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  // False until the map has been moved out of its initial layout.
  bool initialized_ = false;
  // Replaced when its shader is reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_SHADOW_PASS_H_
