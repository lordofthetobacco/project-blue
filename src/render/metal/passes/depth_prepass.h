#ifndef BLUE_RENDER_METAL_PASSES_DEPTH_PREPASS_H_
#define BLUE_RENDER_METAL_PASSES_DEPTH_PREPASS_H_

#include "render/metal/metal_types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "render/metal/material_system.h"
#include "render/metal/mesh.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/pipeline.h"
#include "render/metal/shader_programs.h"
#include "shader/metal/shader_compiler.h"

namespace blue::render::metal {

// Writes the scene depth of the camera view before anything is shaded
// (shaders/depth.vert.metal without a fragment stage; alpha-masked
// materials add depth_masked.frag.metal and bind their material). SSAO
// reads the result and the mesh pass tests EQUAL against it, so every
// visible pixel is shaded exactly once.
//
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class DepthPrepass {
 public:
  DepthPrepass(const MetalContext& context, ShaderPrograms& programs,
               const std::string& shader_dir, const MaterialSystem& materials,
               const std::vector<Mesh>& meshes);

  DepthPrepass(const DepthPrepass&) = delete;
  DepthPrepass& operator=(const DepthPrepass&) = delete;

  // Opens its own encoder on ctx.rec.cmd, clears the depth target (reversed Z:
  // to 0) and draws ctx.draw_list into it.
  void Record(const FrameContext& ctx) const;

 private:
  // Creates the pipelines of the variants selected by `masked` from one
  // compiled program; returns the Metal error text on failure (and keeps
  // the previous pipelines).
  [[nodiscard]] std::optional<std::string> CreatePipelines(
      bool masked, const shader::metal::ShaderLibrary& vertex,
      const shader::metal::ShaderLibrary* fragment);

  const MetalContext& context_;
  const MaterialSystem& materials_;
  const std::vector<Mesh>& meshes_;
  // Indexed by MaterialVariant; replaced when the shaders are reloaded,
  // never null after construction.
  std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> pipelines_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_DEPTH_PREPASS_H_
