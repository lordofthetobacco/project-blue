#ifndef BLUE_RENDER_VULKAN_PASSES_DEPTH_PREPASS_H_
#define BLUE_RENDER_VULKAN_PASSES_DEPTH_PREPASS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "render/vulkan/material_system.h"
#include "render/vulkan/mesh.h"
#include "render/vulkan/passes/frame_context.h"
#include "render/vulkan/pipeline.h"
#include "render/vulkan/shader_programs.h"
#include "render/vulkan/vulkan_context.h"

namespace blue::render::vulkan {

// Writes the scene depth of the camera view before anything is shaded
// (shaders/depth.vert; alpha-masked materials add depth_masked.frag and
// bind their material). SSAO reads the result and the mesh pass tests EQUAL
// against it, so every visible pixel is shaded exactly once.
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class DepthPrepass {
 public:
  DepthPrepass(const VulkanContext& context, ShaderPrograms& programs,
               const std::string& shader_dir,
               VkDescriptorSetLayout frame_layout,
               const MaterialSystem& materials,
               const std::vector<Mesh>& meshes);

  DepthPrepass(const DepthPrepass&) = delete;
  DepthPrepass& operator=(const DepthPrepass&) = delete;

  // Clears the depth target and draws ctx.draw_list into it. Must be
  // recorded outside of a rendering pass. Leaves the depth target in
  // DEPTH_READ_ONLY_OPTIMAL (sampled by SSAO, depth-tested by the scene
  // pass).
  void Record(const FrameContext& ctx) const;

 private:
  // Creates the pipelines of the variants selected by `masked` from one
  // compiled program.
  void CreatePipelines(bool masked, std::span<const uint32_t> vertex,
                       std::span<const uint32_t> fragment);

  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
  // Indexed by MaterialVariant; replaced when the shaders are reloaded,
  // never null after construction.
  std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> pipelines_;
};

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_PASSES_DEPTH_PREPASS_H_
