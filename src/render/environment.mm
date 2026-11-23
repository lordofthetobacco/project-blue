#include "render/environment.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "core/path.h"
#include "render/encoders.h"
#include "render/texture_types.h"

namespace blue::render {

namespace {

constexpr MTLPixelFormat kCubeFormat = MTLPixelFormatRGBA16Float;
constexpr MTLPixelFormat kLutFormat = MTLPixelFormatRG16Float;
// Unfiltered, so the panorama needs no linear-filter support for 32-bit
// floats: the conversion shader filters it by hand with read().
constexpr MTLPixelFormat kEquirectFormat = MTLPixelFormatRGBA32Float;
constexpr uint32_t kFaceCount = 6;

// Size of the default sky (it is smooth, so small is enough).
constexpr int kDefaultSkyWidth = 256;
constexpr int kDefaultSkyHeight = 128;

// GGX samples per texel of every prefiltered mip below mip 0 (mip 0 is a
// copy: roughness 0). Filtered importance sampling (source mips chosen by
// the sample's solid angle) keeps this low without fireflies.
constexpr int kPrefilterSamples = 128;

// Must match the constant blocks of the env_*.frag.metal shaders.
struct ToCubeConstants {
  int32_t face;
  float max_luminance;  // texels above are scaled down to it
  int32_t width;        // of the equirect image
  int32_t height;
};
static_assert(sizeof(ToCubeConstants) == 16);
struct PrefilterConstants {
  int32_t face;
  float roughness;    // perceptual
  float source_size;  // texels per face of source mip 0
  int32_t sample_count;
};
static_assert(sizeof(PrefilterConstants) == 16);

// Every cube is rendered into face by face and sampled. Mip generation of
// the source cube needs no usage flag (only a filterable, colour-renderable
// format).
Image MakeCube(const MetalContext& context, uint32_t size, uint32_t mips,
               const char* label) {
  return Image(context, {.extent = {size, size},
                         .format = kCubeFormat,
                         .usage = MTLTextureUsageRenderTarget |
                                  MTLTextureUsageShaderRead,
                         .array_layers = kFaceCount,
                         .mip_levels = mips,
                         .cube = true,
                         .label = label});
}

core::Extent2D MipExtent(uint32_t size, uint32_t mip) {
  const uint32_t mip_size = std::max(size >> mip, 1u);
  return {mip_size, mip_size};
}

// Returns `equirect` as RGBA32F texels (alpha 1) for the upload.
std::vector<std::byte> ToRgba(const asset::ImageDataF& equirect) {
  const size_t texels = static_cast<size_t>(equirect.width) *
                        static_cast<size_t>(equirect.height);
  constexpr size_t kTexelBytes = 4 * sizeof(float);
  // Written in place: one copy of the panorama, not a float and a byte one.
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

Environment::Environment(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         const Samplers& samplers, FrameGlobals& frame_globals)
    : context_(context),
      samplers_(samplers),
      sky_cube_(MakeCube(context, kSkySize, 1, "Sky cube")),
      source_cube_(MakeCube(context, kSpecularSize,
                            MipLevelCount({kSpecularSize, kSpecularSize}),
                            "Environment source cube")),
      specular_cube_(MakeCube(context, kSpecularSize, kPrefilterMips,
                              "Environment specular cube")),
      brdf_lut_(context, {.extent = {kBrdfLutSize, kBrdfLutSize},
                          .format = kLutFormat,
                          .usage = MTLTextureUsageRenderTarget |
                                   MTLTextureUsageShaderRead,
                          .label = "BRDF LUT"}),
      to_cube_pass_(context, programs, shader_dir,
                    {.fragment_path = core::JoinPath(
                         shader_dir, "env_equirect_to_cube.frag.metal"),
                     .pass_constant_size = sizeof(ToCubeConstants),
                     .output_format = kCubeFormat,
                     .label = "equirect to cube"}),
      prefilter_pass_(
          context, programs, shader_dir,
          {.fragment_path =
               core::JoinPath(shader_dir, "env_prefilter.frag.metal"),
           .pass_constant_size = sizeof(PrefilterConstants),
           .output_format = kCubeFormat,
           .label = "environment prefilter"}),
      brdf_lut_pass_(
          context, programs, shader_dir,
          {.fragment_path =
               core::JoinPath(shader_dir, "env_brdf_lut.frag.metal"),
           .input_count = 0,
           .output_format = kLutFormat,
           .label = "BRDF LUT"}) {
  const std::array<id<MTLTexture>, 1> source{source_cube_.texture()};
  prefilter_pass_.SetInputs(0, source, samplers_.linear_clamp());

  GenerateBrdfLut();
  Set({}, 1.0f, 0.0f);
  frame_globals.BindEnvironment(specular_cube_.texture(), brdf_lut_.texture());
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
  const Image source =
      CreateSampledImage(context_,
                         {.extent = {static_cast<uint32_t>(panorama.width),
                                     static_cast<uint32_t>(panorama.height)},
                          .format = kEquirectFormat,
                          .label = "Environment panorama"},
                         ToRgba(panorama));
  // The shader reads texels with read(); the sampler is never used.
  const std::array<id<MTLTexture>, 1> panorama_input{source.texture()};
  to_cube_pass_.SetInputs(0, panorama_input, samplers_.nearest_clamp());

  const float clamp_luminance = sun_.clamp_luminance;
  const int32_t width = panorama.width;
  const int32_t height = panorama.height;
  context_.SubmitImmediate([&](const Recording& rec) {
    // Sky (unclamped) and specular source (sun clamped) at mip 0.
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      ToCubeConstants constants{
          .face = static_cast<int32_t>(face),
          .max_luminance = std::numeric_limits<float>::max(),
          .width = width,
          .height = height};
      to_cube_pass_.Record(rec,
                             {.texture = sky_cube_.texture(),
                              .slice = face,
                              .extent = {kSkySize, kSkySize}},
                             0, ConstantBytes(constants));
      constants.max_luminance =
          std::min(clamp_luminance, std::numeric_limits<float>::max());
      to_cube_pass_.Record(rec,
                             {.texture = source_cube_.texture(),
                              .slice = face,
                              .extent = {kSpecularSize, kSpecularSize}},
                             0, ConstantBytes(constants));
    }
    RecordSourceMips(rec);
    RecordFiltering(rec);
  });
  // Releases the panorama (it can be large) instead of keeping it alive in
  // the input set until the next Set(); the pass is not recorded until then.
  const std::array<id<MTLTexture>, 1> placeholder{brdf_lut_.texture()};
  to_cube_pass_.SetInputs(0, placeholder, samplers_.nearest_clamp());
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

void Environment::RecordSourceMips(const Recording& rec) const {
  // Box-filters every face down the whole chain, after the render encoders
  // that wrote mip 0 (BeginCopy()'s barrier).
  const id<MTL4ComputeCommandEncoder> encoder =
      BeginCopy(rec, "Environment source mips");
  [encoder generateMipmapsForTexture:source_cube_.texture()];
  [encoder endEncoding];
}

void Environment::RecordFiltering(const Recording& rec) const {
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    const float roughness =
        static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      const PrefilterConstants constants{
          .face = static_cast<int32_t>(face),
          .roughness = roughness,
          .source_size = static_cast<float>(kSpecularSize),
          .sample_count = mip == 0 ? 1 : kPrefilterSamples};
      prefilter_pass_.Record(rec,
                               {.texture = specular_cube_.texture(),
                                .level = mip,
                                .slice = face,
                                .extent = MipExtent(kSpecularSize, mip)},
                               0, ConstantBytes(constants));
    }
  }
}

void Environment::GenerateBrdfLut() {
  const auto start = std::chrono::steady_clock::now();
  context_.SubmitImmediate([&](const Recording& rec) {
    brdf_lut_pass_.Record(rec, {.texture = brdf_lut_.texture(),
                                .extent = {kBrdfLutSize, kBrdfLutSize}});
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

}  // namespace blue::render
