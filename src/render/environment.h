#ifndef BLUE_RENDER_ENVIRONMENT_H_
#define BLUE_RENDER_ENVIRONMENT_H_

#include <vulkan/vulkan.h>

#include <array>
#include <string>
#include <vector>

#include "asset/image_decoder.h"
#include "lighting/environment.h"
#include "render/frame_globals.h"
#include "render/fullscreen_pass.h"
#include "render/image.h"
#include "render/samplers.h"
#include "render/shader_programs.h"
#include "render/vulkan_context.h"

namespace blue::render {

// Image-based lighting on the GPU: turns an equirect HDR panorama into
//   - the sky cube (512^2 RGBA16F, no mips): the panorama as it is, drawn
//     by SkyboxPass;
//   - the specular cube (256^2 RGBA16F, kPrefilterMips mips): GGX-prefiltered
//     for perceptual roughness mip / (kPrefilterMips - 1), bound as
//     FrameGlobals binding 2;
//   - SH9 diffuse irradiance (CPU), written into FrameUniforms;
// plus the split-sum BRDF lookup table (256^2 RG16F, generated on the GPU
// once), FrameGlobals binding 3.
//
// The panorama's sun is too small and bright for the prefiltered cube and
// the SH (fireflies, and it would double the scene's analytic sun). Lighting
// therefore uses the panorama with texels above
// lighting::EstimateSun().clamp_luminance scaled down to it; only the sky
// cube keeps the sun.
//
// Borrows `context`, `samplers` and `frame_globals`, which must outlive it.
// Neither copyable nor movable. Not thread-safe.
class Environment {
 public:
  static constexpr uint32_t kSkySize = 512;
  static constexpr uint32_t kSpecularSize = 256;
  static constexpr uint32_t kPrefilterMips = 6;  // 256 .. 8
  static constexpr uint32_t kBrdfLutSize = 256;

  // Creates the images and pipelines, generates the BRDF LUT, binds the
  // specular cube and LUT into `frame_globals` and sets the generated
  // default sky (lighting::MakeDefaultEnvironment) with intensity 1.
  Environment(const VulkanContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, const Samplers& samplers,
              FrameGlobals& frame_globals);

  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;

  // Replaces the environment by `equirect` (linear radiance, see
  // lighting/environment.h for the mapping) and blocks until the GPU has
  // filtered it. The GPU must be idle (no frame in flight). An empty image
  // sets the default sky.
  void Set(const asset::ImageDataF& equirect, float intensity,
           float rotation_radians);

  // Changes the radiance scale and the rotation about +Y (a feature at
  // direction d shows at angleAxis(rotation, +Y) * d) without refiltering.
  void SetParams(float intensity, float rotation_radians);

  // Writes the SH9 irradiance and environment_params into `uniforms`.
  void FillUniforms(FrameUniforms& uniforms) const;

  // The sky cube (SHADER_READ_ONLY_OPTIMAL) and the sampler to read it.
  [[nodiscard]] VkImageView sky_view() const { return sky_cube_.view(); }
  [[nodiscard]] VkSampler sky_sampler() const {
    return samplers_.linear_clamp();
  }
  // The sun found in the current panorama (found == false for the default
  // sky).
  [[nodiscard]] const lighting::SunEstimate& sun() const { return sun_; }
  // Device memory of the cubes and the BRDF LUT, in bytes.
  [[nodiscard]] VkDeviceSize size_bytes() const {
    return sky_cube_.size_bytes() + source_cube_.size_bytes() +
           specular_cube_.size_bytes() + brdf_lut_.size_bytes();
  }

 private:
  // Records the GGX prefilter of the specular source cube (all mips in
  // SHADER_READ_ONLY_OPTIMAL) into the specular cube.
  void RecordFiltering(VkCommandBuffer cmd) const;
  // Fills the specular source cube's mips by blitting mip 0 down.
  void RecordSourceMips(VkCommandBuffer cmd) const;
  void GenerateBrdfLut();

  const VulkanContext& context_;
  const Samplers& samplers_;
  Image sky_cube_;
  Image source_cube_;  // Clamped panorama with a full mip chain.
  Image specular_cube_;
  Image brdf_lut_;
  std::array<ImageView, 6> sky_face_views_;
  std::array<ImageView, 6> source_face_views_;
  std::vector<ImageView> specular_face_views_;  // [mip * 6 + face]
  ImageView brdf_lut_target_;
  FullscreenPass to_cube_pass_;
  FullscreenPass prefilter_pass_;
  FullscreenPass brdf_lut_pass_;

  lighting::IrradianceSh9 irradiance_;
  lighting::SunEstimate sun_;
  float intensity_ = 1.0f;
  float rotation_radians_ = 0.0f;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_ENVIRONMENT_H_
