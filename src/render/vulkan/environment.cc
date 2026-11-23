#include "render/vulkan/environment.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "render/shader_manifest.h"
#include "core/log.h"
#include "render/vulkan/barriers.h"

namespace blue::render::vulkan {

namespace {

constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16_SFLOAT;
// Unfiltered, so the panorama needs no linear-filter support for 32-bit
// floats: the conversion shader filters it by hand with texelFetch.
constexpr VkFormat kEquirectFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr uint32_t kFaceCount = 6;

// Size of the default sky (it is smooth, so small is enough).
constexpr int kDefaultSkyWidth = 256;
constexpr int kDefaultSkyHeight = 128;

// GGX samples per texel of every prefiltered mip below mip 0 (mip 0 is a
// copy: roughness 0). Filtered importance sampling (source mips chosen by
// the sample's solid angle) keeps this low without fireflies.
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

// Transfer source / destination scopes of the mip blits.
constexpr BarrierScope kTransferRead{VK_PIPELINE_STAGE_2_BLIT_BIT,
                                     VK_ACCESS_2_TRANSFER_READ_BIT};
constexpr BarrierScope kTransferWrite{VK_PIPELINE_STAGE_2_BLIT_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT};

Image MakeCube(const VulkanContext& context, uint32_t size, uint32_t mips,
               VkImageUsageFlags extra_usage) {
  return Image(context, {.extent = {size, size},
                         .format = kCubeFormat,
                         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | extra_usage,
                         .array_layers = kFaceCount,
                         .mip_levels = mips,
                         .cube = true,
                         .dedicated = true});
}

ImageView FaceView(const Image& image, uint32_t mip, uint32_t face) {
  return image.CreateView({.type = VK_IMAGE_VIEW_TYPE_2D,
                           .base_mip = mip,
                           .mip_count = 1,
                           .base_layer = face,
                           .layer_count = 1});
}

VkExtent2D MipExtent(uint32_t size, uint32_t mip) {
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

Environment::Environment(const VulkanContext& context, ShaderPrograms& programs,
                         const Samplers& samplers, FrameGlobals& frame_globals)
    : context_(context),
      samplers_(samplers),
      sky_cube_(MakeCube(context, kSkySize, 1, 0)),
      source_cube_(MakeCube(
          context, kSpecularSize, MipLevelCount({kSpecularSize, kSpecularSize}),
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)),
      specular_cube_(MakeCube(context, kSpecularSize, kPrefilterMips, 0)),
      brdf_lut_(context, {.extent = {kBrdfLutSize, kBrdfLutSize},
                          .format = kLutFormat,
                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT}),
      to_cube_pass_(context, programs,
                    {.program = ShaderProgram::kEnvEquirectToCube,
                     .push_constant_size = sizeof(ToCubeConstants),
                     .output_format = kCubeFormat}),
      prefilter_pass_(
          context, programs,
          {.program = ShaderProgram::kEnvPrefilter,
           .push_constant_size = sizeof(PrefilterConstants),
           .output_format = kCubeFormat}),
      brdf_lut_pass_(
          context, programs,
          {.program = ShaderProgram::kEnvBrdfLut,
           .output_format = kLutFormat}) {
  for (uint32_t face = 0; face < kFaceCount; ++face) {
    sky_face_views_[face] = FaceView(sky_cube_, 0, face);
    source_face_views_[face] = FaceView(source_cube_, 0, face);
  }
  specular_face_views_.reserve(kPrefilterMips * kFaceCount);
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      specular_face_views_.push_back(FaceView(specular_cube_, mip, face));
    }
  }
  brdf_lut_target_ = brdf_lut_.CreateView({});
  const VkImageView source_view = source_cube_.view();
  prefilter_pass_.SetInputs(0, {&source_view, 1}, samplers_.linear_clamp());
  // The LUT shader reads no input; the binding just has to be valid.
  brdf_lut_pass_.SetInputs(0, {&source_view, 1}, samplers_.linear_clamp());

  GenerateBrdfLut();
  Set({}, 1.0f, 0.0f);
  frame_globals.BindEnvironment(specular_cube_.view(), brdf_lut_.view());
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
                          .usage = VK_IMAGE_USAGE_SAMPLED_BIT},
                         ToRgba(panorama));
  const VkImageView source_view = source.view();
  to_cube_pass_.SetInputs(0, {&source_view, 1}, samplers_.nearest_clamp());

  const float clamp_luminance = sun_.clamp_luminance;
  const int32_t width = panorama.width;
  const int32_t height = panorama.height;
  context_.SubmitImmediate([&](VkCommandBuffer cmd) {
    // Sky (unclamped) and specular source (sun clamped) at mip 0.
    ImageBarrier(cmd, sky_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, kNoAccess,
                 kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ImageBarrier(cmd, source_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kNoAccess, kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      ToCubeConstants constants{
          .face = static_cast<int32_t>(face),
          .max_luminance = std::numeric_limits<float>::max(),
          .width = width,
          .height = height};
      to_cube_pass_.Record(cmd,
                           {.view = sky_face_views_[face].handle(),
                            .extent = {kSkySize, kSkySize}},
                           0, PushBytes(constants));
      constants.max_luminance =
          std::min(clamp_luminance, std::numeric_limits<float>::max());
      to_cube_pass_.Record(cmd,
                           {.view = source_face_views_[face].handle(),
                            .extent = {kSpecularSize, kSpecularSize}},
                           0, PushBytes(constants));
    }
    ImageBarrier(cmd, sky_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kColorAttachmentWrite, kFragmentSampledRead,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

void Environment::RecordSourceMips(VkCommandBuffer cmd) const {
  const uint32_t mips = source_cube_.mip_levels();
  for (uint32_t mip = 1; mip < mips; ++mip) {
    const VkImageSubresourceRange previous{VK_IMAGE_ASPECT_COLOR_BIT, mip - 1,
                                           1, 0, kFaceCount};
    const VkImageSubresourceRange current{VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0,
                                          kFaceCount};
    ImageBarrier(cmd, source_cube_.handle(), previous,
                 mip == 1 ? kColorAttachmentWrite : kTransferWrite,
                 kTransferRead,
                 mip == 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                          : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    ImageBarrier(cmd, source_cube_.handle(), current, kNoAccess, kTransferWrite,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const auto source_size =
        static_cast<int32_t>(MipExtent(kSpecularSize, mip - 1).width);
    const auto target_size =
        static_cast<int32_t>(MipExtent(kSpecularSize, mip).width);
    const VkImageBlit blit{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, kFaceCount},
        .srcOffsets = {{0, 0, 0}, {source_size, source_size, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, kFaceCount},
        .dstOffsets = {{0, 0, 0}, {target_size, target_size, 1}},
    };
    vkCmdBlitImage(cmd, source_cube_.handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, source_cube_.handle(),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    ImageBarrier(cmd, source_cube_.handle(), previous, kTransferRead,
                 kFragmentSampledRead, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  const VkImageSubresourceRange last{VK_IMAGE_ASPECT_COLOR_BIT, mips - 1, 1, 0,
                                     kFaceCount};
  ImageBarrier(cmd, source_cube_.handle(), last,
               mips == 1 ? kColorAttachmentWrite : kTransferWrite,
               kFragmentSampledRead,
               mips == 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                         : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Environment::RecordFiltering(VkCommandBuffer cmd) const {
  ImageBarrier(cmd, specular_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kNoAccess, kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  for (uint32_t mip = 0; mip < kPrefilterMips; ++mip) {
    const float roughness =
        static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    for (uint32_t face = 0; face < kFaceCount; ++face) {
      const PrefilterConstants constants{
          .face = static_cast<int32_t>(face),
          .roughness = roughness,
          .source_size = static_cast<float>(kSpecularSize),
          .sample_count = mip == 0 ? 1 : kPrefilterSamples};
      prefilter_pass_.Record(
          cmd,
          {.view = specular_face_views_[mip * kFaceCount + face].handle(),
           .extent = MipExtent(kSpecularSize, mip)},
          0, PushBytes(constants));
    }
  }
  ImageBarrier(cmd, specular_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Environment::GenerateBrdfLut() {
  const auto start = std::chrono::steady_clock::now();
  context_.SubmitImmediate([&](VkCommandBuffer cmd) {
    // The input binding points at the source cube, which must be in a
    // sampled layout even though the shader never reads it.
    ImageBarrier(cmd, source_cube_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kNoAccess, kFragmentSampledRead, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, brdf_lut_.handle(), VK_IMAGE_ASPECT_COLOR_BIT, kNoAccess,
                 kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    brdf_lut_pass_.Record(cmd, {.view = brdf_lut_target_.handle(),
                                .extent = {kBrdfLutSize, kBrdfLutSize}});
    ImageBarrier(cmd, brdf_lut_.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
                 kColorAttachmentWrite, kFragmentSampledRead,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

}  // namespace blue::render::vulkan
