#ifndef BLUE_RENDER_D3D12_ENVIRONMENT_H_
#define BLUE_RENDER_D3D12_ENVIRONMENT_H_

#include <cstdint>

#include "asset/image_decoder.h"
#include "lighting/environment.h"
#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/image.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"
#include "render/frame_uniforms.h"

namespace blue::render::d3d12 {

class D3d12Context;
class FrameGlobals;
class Samplers;

// Image-based lighting on the GPU, the same as the Vulkan backend's
// Environment: turns an equirect HDR panorama into
//   - the sky cube (512^2 RGBA16F, no mips), drawn by SkyboxPass;
//   - the specular cube (256^2 RGBA16F, kPrefilterMips mips),
//     GGX-prefiltered for perceptual roughness mip / (kPrefilterMips - 1),
//     FrameGlobals t2;
//   - SH9 diffuse irradiance (CPU), written into FrameUniforms;
// plus the split-sum BRDF lookup table (256^2 RG16F, generated once),
// FrameGlobals t3. Lighting uses the panorama with its sun clamped
// (lighting::EstimateSun()); only the sky cube keeps the sun. The specular
// source cube's mip chain is made with env_downsample (a 2 x 2 box, what the
// Vulkan backend's linear blit computes), as Direct3D 12 has no blit.
//
// Between Set() calls every image is in PIXEL_SHADER_RESOURCE. Borrows
// everything it is given. Neither copyable nor movable. Not thread-safe.
class Environment {
 public:
  static constexpr uint32_t kSkySize = 512;
  static constexpr uint32_t kSpecularSize = 256;
  static constexpr uint32_t kPrefilterMips = 6;  // 256 .. 8
  static constexpr uint32_t kBrdfLutSize = 256;

  // Creates the images and pipelines, generates the BRDF LUT, binds the
  // specular cube and LUT into `frame_globals` and sets the generated
  // default sky (lighting::MakeDefaultEnvironment) with intensity 1.
  Environment(const D3d12Context& context, ShaderPrograms& programs,
              const RootSignature& fullscreen_root, const Samplers& samplers,
              FrameGlobals& frame_globals);

  Environment(const Environment&) = delete;
  Environment& operator=(const Environment&) = delete;

  // Replaces the environment by `equirect` (linear radiance) and blocks
  // until the GPU has filtered it. No frame may be in flight. An empty
  // image sets the default sky.
  void Set(const asset::ImageDataF& equirect, float intensity,
           float rotation_radians);
  // Changes the radiance scale and the rotation about +Y without
  // refiltering.
  void SetParams(float intensity, float rotation_radians);
  // Writes the SH9 irradiance and environment_params into `uniforms`.
  void FillUniforms(FrameUniforms& uniforms) const;

  // The sky cube (PIXEL_SHADER_RESOURCE).
  [[nodiscard]] const Image& sky() const { return sky_cube_; }
  // Device memory of the cubes and the BRDF LUT, in bytes.
  [[nodiscard]] uint64_t size_bytes() const {
    return sky_cube_.size_bytes() + source_cube_.size_bytes() +
           specular_cube_.size_bytes() + brdf_lut_.size_bytes();
  }

 private:
  // Records the source cube's mips 1.. from mip 0 (written, RENDER_TARGET);
  // leaves every mip in PIXEL_SHADER_RESOURCE.
  void RecordSourceMips(ID3D12GraphicsCommandList* cmd);
  // Records the GGX prefilter of the source cube into the specular cube.
  void RecordFiltering(ID3D12GraphicsCommandList* cmd);
  void GenerateBrdfLut();

  const D3d12Context& context_;
  const Samplers& samplers_;
  // Render target views: rtv(face) of the sky cube; rtv(mip * 6 + face) of
  // the source and specular cubes; rtv(0) of the LUT. mip_srv(m) of the
  // source cube: mip m, every face.
  Image sky_cube_;
  Image source_cube_;  // Clamped panorama with a full mip chain.
  Image specular_cube_;
  Image brdf_lut_;
  FullscreenPass to_cube_pass_;
  FullscreenPass downsample_pass_;
  FullscreenPass prefilter_pass_;
  FullscreenPass brdf_lut_pass_;
  lighting::IrradianceSh9 irradiance_;
  lighting::SunEstimate sun_;
  float intensity_ = 1.0f;
  float rotation_radians_ = 0.0f;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_ENVIRONMENT_H_
