#include "render/d3d12/environment.h"

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "lighting/environment.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/format.h"
#include "render/d3d12/frame_globals.h"
#include "render/d3d12/image.h"
#include "render/d3d12/samplers.h"
#include "render/d3d12/texture_cache.h"
#include "render/shader_manifest.h"
#include "render/texture_types.h"

namespace blue::render::d3d12 {

namespace {

constexpr DXGI_FORMAT kCubeFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kLutFormat = DXGI_FORMAT_R16G16_FLOAT;
// Unfiltered: the conversion shader filters it by hand with Load.
constexpr DXGI_FORMAT kEquirectFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
constexpr uint32_t kFaceCount = 6;

// The default sky (smooth, so small is enough).
constexpr int kDefaultSkyWidth = 256;
constexpr int kDefaultSkyHeight = 128;

// GGX samples per texel of every prefiltered mip below mip 0.
constexpr int kPrefilterSamples = 128;

// Must match the push constant blocks of shaders/env_*.slang.
struct ToCubeConstants {
  int32_t face;
  float max_luminance;  // texels above are scaled down to it
  int32_t width;        // of the equirect image
  int32_t height;
};
struct PrefilterConstants {
  int32_t face;
  float roughness;    // perceptual
  float source_size;  // texels per face of source mip 0
  int32_t sample_count;
};
struct DownsampleConstants {
  float texel_size[2];  // 1 / target size
  int32_t face;
  int32_t padding;
};

template <typename T>
std::span<const std::byte> Bytes(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}

uint32_t MipSize(uint32_t size, uint32_t mip) { return std::max(size >> mip, 1u); }

// A cube render target with a render target view per mip and face, its
// staging SRV (TextureCube) and every subresource discarded (initialized),
// in RENDER_TARGET.
Image MakeCube(const D3d12Context& context, uint32_t size, uint32_t mips,
               const wchar_t* name) {
  Image cube(context, {.extent = {size, size},
                       .mip_levels = mips,
                       .layers = kFaceCount,
                       .cube = true,
                       .format = kCubeFormat,
                       .flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                       .initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
                       .name = name});
  for (uint32_t mip = 0; mip < mips; ++mip) {
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      cube.AddRtv(kCubeFormat, mip, face);
    }
  }
  cube.CreateSrv(kCubeFormat, /*shader_visible=*/false);
  return cube;
}

// Returns `equirect` as RGBA32F texels (alpha 1) for the upload.
std::vector<std::byte> ToRgba(const asset::ImageDataF& equirect) {
  const size_t texels = static_cast<size_t>(equirect.width) *
                        static_cast<size_t>(equirect.height);
  constexpr size_t kTexelBytes = 4 * sizeof(float);
  std::vector<std::byte> bytes(texels * kTexelBytes);
  const float* source = equirect.pixels.data();
  std::byte* target = bytes.data();
  for (size_t i = 0; i < texels; ++i, source += 3, target += kTexelBytes) {
    const std::array<float, 4> texel = {source[0], source[1], source[2], 1.0f};
    std::memcpy(target, texel.data(), kTexelBytes);
  }
  return bytes;
}

}  // namespace

Environment::Environment(const D3d12Context& context, ShaderPrograms& programs,
                         const RootSignature& fullscreen_root,
                         const Samplers& samplers, FrameGlobals& frame_globals)
    : context_(context),
      samplers_(samplers),
      sky_cube_(MakeCube(context, kSkySize, 1, L"blue sky cube")),
      source_cube_(MakeCube(context, kSpecularSize,
                            MipLevelCount({kSpecularSize, kSpecularSize}),
                            L"blue specular source cube")),
      specular_cube_(MakeCube(context, kSpecularSize, kPrefilterMips,
                              L"blue specular cube")),
      to_cube_pass_(context, programs, fullscreen_root,
                    ShaderProgram::kEnvEquirectToCube, kCubeFormat),
      downsample_pass_(context, programs, fullscreen_root,
                       ShaderProgram::kEnvDownsample, kCubeFormat),
      prefilter_pass_(context, programs, fullscreen_root,
                      ShaderProgram::kEnvPrefilter, kCubeFormat),
      brdf_lut_pass_(context, programs, fullscreen_root,
                     ShaderProgram::kEnvBrdfLut, kLutFormat) {
  brdf_lut_ = Image(context, {.extent = {kBrdfLutSize, kBrdfLutSize},
                              .format = kLutFormat,
                              .flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                              .initial_state = D3D12_RESOURCE_STATE_RENDER_TARGET,
                              .name = L"blue brdf lut"});
  brdf_lut_.AddRtv(kLutFormat);
  brdf_lut_.CreateSrv(kLutFormat, /*shader_visible=*/false);
  for (uint32_t mip = 0; mip + 1 < source_cube_.mip_levels(); ++mip) {
    source_cube_.AddMipSrv(kCubeFormat, mip);
  }
  // Placed render targets must be initialized before anything uses them;
  // every texel is written before it is read, so discarding is enough.
  context_.SubmitImmediate([this](ID3D12GraphicsCommandList* cmd) {
    for (const Image* image :
         {&sky_cube_, &source_cube_, &specular_cube_, &brdf_lut_}) {
      cmd->DiscardResource(image->resource(), nullptr);
    }
  });

  GenerateBrdfLut();
  Set({}, 1.0f, 0.0f);
  frame_globals.BindEnvironment(specular_cube_, brdf_lut_);
}

void Environment::Set(const asset::ImageDataF& equirect, float intensity,
                      float rotation_radians) {
  const auto start = std::chrono::steady_clock::now();
  const bool use_default =
      equirect.width <= 0 || equirect.height <= 0 ||
      equirect.pixels.size() != static_cast<size_t>(equirect.width) *
                                    static_cast<size_t>(equirect.height) * 3;
  const asset::ImageDataF default_sky =
      use_default ? lighting::MakeDefaultEnvironment(kDefaultSkyWidth,
                                                     kDefaultSkyHeight)
                  : asset::ImageDataF{};
  const asset::ImageDataF& panorama = use_default ? default_sky : equirect;

  sun_ = lighting::EstimateSun(panorama);
  irradiance_ =
      lighting::ComputeIrradianceSh9(panorama, 256, sun_.clamp_luminance);
  const std::vector<std::byte> texels = ToRgba(panorama);
  const std::array<std::span<const std::byte>, 1> subresources = {texels};
  const Image source = CreateSampledImage(
      context_,
      {.extent = {static_cast<uint32_t>(panorama.width),
                  static_cast<uint32_t>(panorama.height)},
       .format = kEquirectFormat,
       .name = L"blue equirect"},
      {.extent = 1, .bytes = 16}, subresources);

  const float clamp_luminance = sun_.clamp_luminance;
  const int32_t width = panorama.width;
  const int32_t height = panorama.height;
  context_.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    {
      BarrierBatch barriers(cmd);
      sky_cube_.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
      source_cube_.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        source.staging_srv()};
    // Sky (unclamped) and specular source (sun clamped) at mip 0.
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      ToCubeConstants constants{
          .face = static_cast<int32_t>(face),
          .max_luminance = std::numeric_limits<float>::max(),
          .width = width,
          .height = height};
      to_cube_pass_.Record(cmd, sky_cube_.rtv(face), {kSkySize, kSkySize},
                           inputs, samplers_.nearest_clamp(), Bytes(constants));
      constants.max_luminance =
          std::min(clamp_luminance, std::numeric_limits<float>::max());
      to_cube_pass_.Record(cmd, source_cube_.rtv(face),
                           {kSpecularSize, kSpecularSize}, inputs,
                           samplers_.nearest_clamp(), Bytes(constants));
    }
    {
      BarrierBatch barriers(cmd);
      sky_cube_.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    RecordSourceMips(cmd);
    RecordFiltering(cmd);
  });
  intensity_ = intensity;
  rotation_radians_ = rotation_radians;

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  core::log::Info(
      "environment set: {} x {}{}, sun {} (clamp at luminance {:.1f}), "
      "{:.1f} ms",
      panorama.width, panorama.height, use_default ? " (default sky)" : "",
      sun_.found ? "found" : "none", sun_.clamp_luminance, elapsed.count());
}

void Environment::RecordSourceMips(ID3D12GraphicsCommandList* cmd) {
  const uint32_t mips = source_cube_.mip_levels();
  const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  for (uint32_t mip = 1; mip < mips; ++mip) {
    {
      // The previous mip becomes readable; this one stays a render target.
      BarrierBatch barriers(cmd);
      for (uint32_t face = 0; face < kFaceCount; ++face) {
        barriers.Transition(source_cube_.resource(),
                            D3D12_RESOURCE_STATE_RENDER_TARGET, kRead,
                            source_cube_.subresource(mip - 1, face));
      }
    }
    const uint32_t size = MipSize(kSpecularSize, mip);
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        source_cube_.mip_srv(mip - 1)};
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      const DownsampleConstants constants{
          .texel_size = {1.0f / static_cast<float>(size),
                         1.0f / static_cast<float>(size)},
          .face = static_cast<int32_t>(face),
          .padding = 0};
      downsample_pass_.Record(cmd, source_cube_.rtv(mip * kFaceCount + face),
                              {size, size}, inputs, samplers_.linear_clamp(),
                              Bytes(constants));
    }
  }
  BarrierBatch barriers(cmd);
  for (uint32_t face = 0; face < kFaceCount; ++face) {
    barriers.Transition(source_cube_.resource(),
                        D3D12_RESOURCE_STATE_RENDER_TARGET, kRead,
                        source_cube_.subresource(mips - 1, face));
  }
  source_cube_.AssumeState(kRead);
}

void Environment::RecordFiltering(ID3D12GraphicsCommandList* cmd) {
  {
    BarrierBatch barriers(cmd);
    specular_cube_.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
      source_cube_.staging_srv()};
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    const float roughness =
        static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    const uint32_t size = MipSize(kSpecularSize, mip);
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      const PrefilterConstants constants{
          .face = static_cast<int32_t>(face),
          .roughness = roughness,
          .source_size = static_cast<float>(kSpecularSize),
          .sample_count = mip == 0 ? 1 : kPrefilterSamples};
      prefilter_pass_.Record(cmd, specular_cube_.rtv(mip * kFaceCount + face),
                             {size, size}, inputs, samplers_.linear_clamp(),
                             Bytes(constants));
    }
  }
  BarrierBatch barriers(cmd);
  specular_cube_.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void Environment::GenerateBrdfLut() {
  const auto start = std::chrono::steady_clock::now();
  context_.SubmitImmediate([&](ID3D12GraphicsCommandList* cmd) {
    // The LUT shader reads no input.
    brdf_lut_pass_.Record(cmd, brdf_lut_.rtv(), {kBrdfLutSize, kBrdfLutSize},
                          {}, samplers_.linear_clamp(), {});
    BarrierBatch barriers(cmd);
    brdf_lut_.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  });
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  core::log::Info("BRDF LUT generated on the GPU in {:.1f} ms",
                  elapsed.count());
}

void Environment::SetParams(float intensity, float rotation_radians) {
  intensity_ = intensity;
  rotation_radians_ = rotation_radians;
}

void Environment::FillUniforms(FrameUniforms& uniforms) const {
  for (size_t i = 0; i < irradiance_.coefficients.size(); ++i) {
    uniforms.irradiance_sh[i] = glm::vec4(irradiance_.coefficients[i], 0.0f);
  }
  uniforms.environment_params =
      glm::vec4(intensity_, rotation_radians_,
                static_cast<float>(kPrefilterMips - 1), 1.0f);
}

}  // namespace blue::render::d3d12
