#include "render/vulkan/descriptor_layout.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <vector>

#include "render/binding_layout.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

VkDescriptorType ToVkDescriptorType(BindingKind kind) {
  switch (kind) {
    case BindingKind::kUniformBuffer:
      return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingKind::kTexture:
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingKind::kSampler:
      return VK_DESCRIPTOR_TYPE_SAMPLER;
  }
  return VK_DESCRIPTOR_TYPE_SAMPLER;
}

VkDescriptorSetLayout CreateSetLayout(VkDevice device,
                                      std::span<const BindingDesc> bindings) {
  std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
  vk_bindings.reserve(bindings.size());
  for (const BindingDesc& binding : bindings) {
    VkShaderStageFlags stages = 0;
    if ((binding.stages & kStageVertex) != 0) {
      stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((binding.stages & kStageFragment) != 0) {
      stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    vk_bindings.push_back({
        .binding = binding.binding,
        .descriptorType = ToVkDescriptorType(binding.kind),
        .descriptorCount = 1,
        .stageFlags = stages,
    });
  }
  const VkDescriptorSetLayoutCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(vk_bindings.size()),
      .pBindings = vk_bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout));
  return layout;
}

VkWriteDescriptorSet ImageWrite(VkDescriptorSet set, uint32_t binding,
                                const VkDescriptorImageInfo& info) {
  return {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .pImageInfo = &info,
  };
}

VkWriteDescriptorSet SamplerWrite(VkDescriptorSet set, uint32_t binding,
                                  const VkDescriptorImageInfo& info) {
  return {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = set,
      .dstBinding = binding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
      .pImageInfo = &info,
  };
}

}  // namespace blue::render::vulkan
