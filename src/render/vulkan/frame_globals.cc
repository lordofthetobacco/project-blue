#include "render/vulkan/frame_globals.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "render/binding_layout.h"
#include "render/vulkan/descriptor_layout.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

constexpr uint32_t kUniformBinding = BLUE_FRAME_UNIFORMS_BINDING;
constexpr uint32_t kShadowMapBinding = BLUE_FRAME_SHADOW_MAP_BINDING;
constexpr uint32_t kEnvironmentBinding = BLUE_FRAME_ENVIRONMENT_BINDING;
constexpr uint32_t kBrdfLutBinding = BLUE_FRAME_BRDF_LUT_BINDING;
constexpr uint32_t kAmbientOcclusionBinding = BLUE_FRAME_AO_BINDING;
constexpr uint32_t kShadowSamplerBinding = BLUE_FRAME_SHADOW_SAMPLER_BINDING;
constexpr uint32_t kLinearSamplerBinding = BLUE_FRAME_LINEAR_SAMPLER_BINDING;
constexpr uint32_t kImageBindingCount = 4;
constexpr uint32_t kSamplerBindingCount = 2;

VkDescriptorPool CreatePool(VkDevice device) {
  const auto frames = static_cast<uint32_t>(kFramesInFlight * kFrameViewCount);
  const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frames},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       .descriptorCount = frames * kImageBindingCount},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLER,
       .descriptorCount = frames * kSamplerBindingCount},
  }};
  const VkDescriptorPoolCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = frames,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
  };
  VkDescriptorPool pool = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorPool(device, &create_info, nullptr, &pool));
  return pool;
}

// Returns a 1 x 1 image (6 layers for a cube) whose texels are `texel`.
Image MakeDefaultImage(const VulkanContext& context, VkFormat format,
                       std::span<const std::byte> texel, bool cube) {
  const uint32_t layers = cube ? 6 : 1;
  std::array<std::byte, 6 * 16> texels{};
  for (uint32_t layer = 0; layer < layers; ++layer) {
    std::memcpy(texels.data() + layer * texel.size(), texel.data(),
                texel.size());
  }
  return CreateSampledImage(context,
                            {.extent = {1, 1},
                             .format = format,
                             .array_layers = layers,
                             .cube = cube},
                            std::span(texels).first(texel.size() * layers));
}

}  // namespace

FrameGlobals::FrameGlobals(const VulkanContext& context,
                           const Samplers& samplers)
    : device_(context.device()),
      samplers_(samplers),
      layout_(CreateSetLayout(device_, kFrameBindings)),
      pool_(CreatePool(device_)) {
  constexpr std::array<std::byte, 8> kZeroHalf4{};
  constexpr std::array<std::byte, 4> kZeroHalf2{};
  constexpr std::array<std::byte, 1> kWhiteR8{std::byte{255}};
  default_cube_ = MakeDefaultImage(context, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   kZeroHalf4, /*cube=*/true);
  default_lut_ = MakeDefaultImage(context, VK_FORMAT_R16G16_SFLOAT, kZeroHalf2,
                                  /*cube=*/false);
  default_ambient_occlusion_ =
      MakeDefaultImage(context, VK_FORMAT_R8_UNORM, kWhiteR8, /*cube=*/false);

  for (Frame& frame : frames_) {
    frame.uniforms = Buffer(context.allocator(), sizeof(FrameUniforms),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            Buffer::Access::kCpuSequentialWrite);
    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout_,
    };
    CheckVk(vkAllocateDescriptorSets(device_, &set_info, &frame.set));
    const VkDescriptorBufferInfo buffer_info{
        .buffer = frame.uniforms.handle(),
        .offset = 0,
        .range = sizeof(FrameUniforms),
    };
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.set,
        .dstBinding = kUniformBinding,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .pBufferInfo = &buffer_info,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }
  BindSampler(kLinearSamplerBinding, samplers_.linear_clamp());
  BindEnvironment(default_cube_.view(), default_lut_.view());
  BindImage(kAmbientOcclusionBinding, default_ambient_occlusion_.view(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

FrameGlobals::~FrameGlobals() {
  vkDestroyDescriptorPool(device_, pool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
}

void FrameGlobals::Write(size_t frame, const FrameUniforms& uniforms,
                         FrameView view) const {
  std::memcpy(frames_[Index(frame, view)].uniforms.mapped(), &uniforms,
              sizeof(uniforms));
}

void FrameGlobals::BindShadowMap(VkImageView view, VkSampler compare_sampler) {
  BindImage(kShadowMapBinding, view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
  BindSampler(kShadowSamplerBinding, compare_sampler);
}

void FrameGlobals::BindEnvironment(VkImageView prefiltered_cube,
                                   VkImageView brdf_lut) {
  BindImage(kEnvironmentBinding, prefiltered_cube,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  BindImage(kBrdfLutBinding, brdf_lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void FrameGlobals::BindAmbientOcclusion(VkImageView ambient_occlusion) {
  BindImage(kAmbientOcclusionBinding, ambient_occlusion,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, FrameView::kMain);
}

void FrameGlobals::BindImage(uint32_t binding, VkImageView view,
                             VkImageLayout layout,
                             std::optional<FrameView> only) {
  const VkDescriptorImageInfo image_info{
      .imageView = view,
      .imageLayout = layout,
  };
  for (size_t i = 0; i < frames_.size(); ++i) {
    if (only && static_cast<size_t>(*only) != i % kFrameViewCount) {
      continue;
    }
    const VkWriteDescriptorSet write =
        ImageWrite(frames_[i].set, binding, image_info);
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }
}

void FrameGlobals::BindSampler(uint32_t binding, VkSampler sampler) {
  const VkDescriptorImageInfo sampler_info{.sampler = sampler};
  for (const Frame& frame : frames_) {
    const VkWriteDescriptorSet write =
        SamplerWrite(frame.set, binding, sampler_info);
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }
}

}  // namespace blue::render::vulkan
