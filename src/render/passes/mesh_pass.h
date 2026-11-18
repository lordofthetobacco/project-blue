#ifndef BLUE_RENDER_PASSES_MESH_PASS_H_
#define BLUE_RENDER_PASSES_MESH_PASS_H_

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "render/material_system.h"
#include "render/mesh.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Shades the frame's draw list with the PBR shaders (shaders/mesh.vert/
// .frag). The depth pre-pass has already written the scene depth, so this
// pass tests EQUAL without writing depth: every pixel is shaded once. Set 0
// is FrameGlobals', set 1 the material's; the push constant is the model
// matrix (the normal matrix is derived in the vertex shader).
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class MeshPass {
 public:
  MeshPass(const VulkanContext& context, ShaderPrograms& programs,
           const std::string& shader_dir, VkDescriptorSetLayout frame_layout,
           const MaterialSystem& materials, const std::vector<Mesh>& meshes);

  MeshPass(const MeshPass&) = delete;
  MeshPass& operator=(const MeshPass&) = delete;

  // Records ctx.draw_list. Call inside the scene pass's rendering
  // (BeginScenePass); leaves an arbitrary pipeline bound.
  void Draw(const FrameContext& ctx) const;

 private:
  VkDevice device_;
  VkDescriptorSetLayout frame_layout_;
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
  // Indexed by MaterialVariant; replaced when the shaders are reloaded,
  // never null after construction.
  std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> pipelines_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_MESH_PASS_H_
