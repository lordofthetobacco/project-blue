#include "render/frame_globals.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "render/vk_check.h"

namespace blue::render {

namespace {

constexpr uint32_t kUniformBinding = 0;
constexpr uint32_t kShadowMapBinding = 1;
constexpr uint32_t kEnvironmentBinding = 2;
constexpr uint32_t kBrdfLutBinding = 3;
constexpr uint32_t kAmbientOcclusionBinding = 4;
constexpr uint32_t kImageBindingCount = 4;

VkDescriptorSetLayout CreateLayout(VkDevice device) {
  std::array<VkDescriptorSetLayoutBinding, 1 + kImageBindingCount> bindings{};
  bindings[0] = {
      .binding = kUniformBinding,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  for (uint32_t i = 1; i < bindings.size(); ++i) {
    bindings[i] = {
        .binding = i,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
  }
  const VkDescriptorSetLayoutCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout));
  return layout;
}

VkDescriptorPool CreatePool(VkDevice device) {
  const auto frames = static_cast<uint32_t>(kFramesInFlight);
  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frames},
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = frames * kImageBindingCount},
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
      layout_(CreateLayout(device_)),
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
  BindEnvironment(default_cube_.view(), default_lut_.view());
  BindAmbientOcclusion(default_ambient_occlusion_.view());
}

FrameGlobals::~FrameGlobals() {
  vkDestroyDescriptorPool(device_, pool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
}

void FrameGlobals::Write(size_t frame, const FrameUniforms& uniforms) const {
  std::memcpy(frames_[frame].uniforms.mapped(), &uniforms, sizeof(uniforms));
}

void FrameGlobals::BindShadowMap(VkImageView view, VkSampler compare_sampler) {
  BindImage(kShadowMapBinding, view, compare_sampler,
            VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
}

void FrameGlobals::BindEnvironment(VkImageView prefiltered_cube,
                                   VkImageView brdf_lut) {
  BindImage(kEnvironmentBinding, prefiltered_cube, samplers_.linear_clamp(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  BindImage(kBrdfLutBinding, brdf_lut, samplers_.linear_clamp(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void FrameGlobals::BindAmbientOcclusion(VkImageView ambient_occlusion) {
  BindImage(kAmbientOcclusionBinding, ambient_occlusion,
            samplers_.linear_clamp(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void FrameGlobals::BindImage(uint32_t binding, VkImageView view,
                             VkSampler sampler, VkImageLayout layout) {
  const VkDescriptorImageInfo image_info{
      .sampler = sampler,
      .imageView = view,
      .imageLayout = layout,
  };
  for (const Frame& frame : frames_) {
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }
}

}  // namespace blue::render
