#ifndef BLUE_RENDER_VULKAN_DESCRIPTOR_LAYOUT_H_
#define BLUE_RENDER_VULKAN_DESCRIPTOR_LAYOUT_H_

#include <vulkan/vulkan.h>

#include <span>

#include "render/binding_layout.h"

namespace blue::render::vulkan {

// UNIFORM_BUFFER, SAMPLED_IMAGE or SAMPLER.
[[nodiscard]] VkDescriptorType ToVkDescriptorType(BindingKind kind);

// Creates the descriptor set layout of `bindings` (one descriptor each, with
// the stages of the table). The caller owns it. Failure is fatal.
[[nodiscard]] VkDescriptorSetLayout CreateSetLayout(
    VkDevice device, std::span<const BindingDesc> bindings);

// Returns a write of the sampled image `info` (its sampler is ignored) to
// `binding` of `set`. `info` must outlive the vkUpdateDescriptorSets call.
[[nodiscard]] VkWriteDescriptorSet ImageWrite(
    VkDescriptorSet set, uint32_t binding, const VkDescriptorImageInfo& info);

// Returns a write of the sampler in `info` to `binding` of `set`. `info`
// must outlive the vkUpdateDescriptorSets call.
[[nodiscard]] VkWriteDescriptorSet SamplerWrite(
    VkDescriptorSet set, uint32_t binding, const VkDescriptorImageInfo& info);

}  // namespace blue::render::vulkan

#endif  // BLUE_RENDER_VULKAN_DESCRIPTOR_LAYOUT_H_
