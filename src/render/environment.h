#ifndef BLUE_RENDER_ENVIRONMENT_H_
#define BLUE_RENDER_ENVIRONMENT_H_

#include "render/metal_types.h"

#include <cstdint>
#include <string>

#include "asset/image_decoder.h"
#include "lighting/environment.h"
#include "render/frame_globals.h"
#include "render/frame_uniforms.h"
#include "render/fullscreen_pass.h"
#include "render/image.h"
#include "render/metal_context.h"
#include "render/samplers.h"
#include "render/shader_programs.h"

namespace blue::render {

// Image-based lighting on the GPU: turns an equirect HDR panorama into
//   - the sky cube (512^2 RGBA16Float, no mips): the panorama as it is,
//     drawn by SkyboxPass;
//   - the specular cube (256^2 RGBA16Float, kPrefilterMips mips):
//     GGX-prefiltered for perceptual roughness mip / (kPrefilterMips - 1),
//     bound through FrameGlobals at kTextureEnvironment;
//   - SH9 diffuse irradiance (CPU), written into FrameUniforms;
// plus the split-sum BRDF lookup table (256^2 RG16Float, generated on the
// GPU once), bound through FrameGlobals at kTextureBrdfLut.
//
// The panorama's sun is too small and bright for the prefiltered cube and
// the SH (fireflies, and it would double the scene's analytic sun). Lighting
// therefore uses the panorama with texels above
// lighting::EstimateSun().clamp_luminance scaled down to it; only the sky
// cube keeps the sun.
//
// The cubes are created once and refilled in place by Set(), so the
// textures handed out (sky_texture(), the FrameGlobals bindings) never
// change. Borrows `context` and `samplers`, which must outlive it. Neither
// copyable nor movable. Not thread-safe.
class Environment {
 public:
  static constexpr uint32_t kSkySize = 512;
  static constexpr uint32_t kSpecularSize = 256;
  static constexpr uint32_t kPrefilterMips = 6;  // 256 .. 8
  static constexpr uint32_t kBrdfLutSize = 256;

  // Creates the images and pipelines, generates the BRDF LUT, binds the
  // specular cube and LUT into `frame_globals` and sets the generated
  // default sky (lighting::MakeDefaultEnvironment) with intensity 1.
  Environment(const MetalContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, const Samplers& samplers,
              FrameGlobals& frame_globals);

  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;

  // Replaces the environment by `equirect` (linear radiance, see
  // lighting/environment.h for the mapping) and blocks until the GPU has
  // filtered it. The GPU must be idle (no frame in flight): the cubes are
  // rewritten in place. An empty image sets the default sky.
  void Set(const asset::ImageDataF& equirect, float intensity,
           float rotation_radians);

  // Changes the radiance scale and the rotation about +Y (a feature at
  // direction d shows at angleAxis(rotation, +Y) * d) without refiltering.
  void SetParams(float intensity, float rotation_radians);

  // Writes the SH9 irradiance and environment_params into `uniforms`.
  void FillUniforms(FrameUniforms& uniforms) const;

  // The sky cube and the sampler to read it.
  [[nodiscard]] id<MTLTexture> sky_texture() const {
    return sky_cube_.texture();
  }
  [[nodiscard]] id<MTLSamplerState> sky_sampler() const {
    return samplers_.linear_clamp();
  }
  // The sun found in the current panorama (found == false for the default
  // sky).
  [[nodiscard]] const lighting::SunEstimate& sun() const { return sun_; }
  // GPU memory of the cubes and the BRDF LUT, in bytes.
  [[nodiscard]] uint64_t size_bytes() const {
    return sky_cube_.size_bytes() + source_cube_.size_bytes() +
           specular_cube_.size_bytes() + brdf_lut_.size_bytes();
  }

 private:
  // Records the GGX prefilter of the specular source cube (with its mips)
  // into the specular cube.
  void RecordFiltering(id<MTLCommandBuffer> cmd) const;
  // Fills the specular source cube's mips from mip 0.
  void RecordSourceMips(id<MTLCommandBuffer> cmd) const;
  void GenerateBrdfLut();

  const MetalContext& context_;
  const Samplers& samplers_;
  Image sky_cube_;
  Image source_cube_;  // Clamped panorama with a full mip chain.
  Image specular_cube_;
  Image brdf_lut_;
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
