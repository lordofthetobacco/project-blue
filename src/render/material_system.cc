#include "render/material_system.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "render/frame_config.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

static_assert(offsetof(MaterialParams, emissive) == 16);
static_assert(offsetof(MaterialParams, surface) == 32);
static_assert(offsetof(MaterialParams, uv_alpha) == 48);
static_assert(sizeof(MaterialParams) == 64);

constexpr uint32_t kParamsBinding = 0;
constexpr uint32_t kTextureCount = 5;  // Bindings 1-5.
constexpr uint32_t kSetsPerPool = 64;

VkDescriptorSetLayout CreateLayout(VkDevice device) {
  std::array<VkDescriptorSetLayoutBinding, 1 + kTextureCount> bindings{};
  bindings[0] = {
      .binding = kParamsBinding,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
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
  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = kSetsPerPool},
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = kSetsPerPool * kTextureCount},
  }};
  const VkDescriptorPoolCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = kSetsPerPool,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
  };
  VkDescriptorPool pool = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorPool(device, &create_info, nullptr, &pool));
  return pool;
}

}  // namespace

MaterialVariant VariantOf(const material::MaterialDesc& desc) {
  const bool masked = desc.alpha_mode == material::AlphaMode::kMask;
  if (desc.double_sided) {
    return masked ? MaterialVariant::kMaskedDoubleSided
                  : MaterialVariant::kDoubleSided;
  }
  return masked ? MaterialVariant::kMasked : MaterialVariant::kOpaque;
}

MaterialParams MakeMaterialParams(const material::MaterialDesc& desc) {
  return {
      .base_color = desc.base_color,
      .emissive = glm::vec4(desc.emissive * desc.emissive_strength, 0.0f),
      .surface = glm::vec4(desc.metallic, desc.roughness, desc.normal_scale,
                           desc.occlusion_strength),
      .uv_alpha = glm::vec4(desc.uv_scale, desc.alpha_cutoff, 0.0f),
  };
}

MaterialSystem::MaterialSystem(const VulkanContext& context,
                               TextureCache& textures, const Samplers& samplers)
    : context_(context),
      textures_(textures),
      samplers_(samplers),
      layout_(CreateLayout(context.device())) {}

MaterialSystem::~MaterialSystem() {
  const VkDevice device = context_.device();
  for (const VkDescriptorPool pool : pools_) {
    vkDestroyDescriptorPool(device, pool, nullptr);
  }
  vkDestroyDescriptorSetLayout(device, layout_, nullptr);
}

MaterialId MaterialSystem::Create(const material::MaterialDesc& desc) {
  Material& added = materials_.emplace_back();
  added.desc = material::Sanitize(desc);
  added.variant = VariantOf(added.desc);
  added.binding = AcquireBinding();
  Write(added.binding, added.desc);
  return static_cast<MaterialId>(materials_.size() - 1);
}

void MaterialSystem::Update(MaterialId id, const material::MaterialDesc& desc) {
  const auto index = static_cast<size_t>(id);
  if (index >= materials_.size()) {
    core::log::Fatal("Update of unknown material {}", index);
  }
  Material& updated = materials_[index];
  Binding fresh = AcquireBinding();
  updated.desc = material::Sanitize(desc);
  updated.variant = VariantOf(updated.desc);
  Write(fresh, updated.desc);
  // Frames up to the current one may have recorded the old set.
  retired_.push_back(
      {.binding = std::exchange(updated.binding, std::move(fresh)),
       .last_frame = frame_serial_});
}

void MaterialSystem::BeginFrame(uint64_t frame_serial) {
  frame_serial_ = frame_serial;
  // Frames up to frame_serial - kFramesInFlight are complete.
  const auto reusable = [frame_serial](const Retired& retired) {
    return retired.last_frame + kFramesInFlight <= frame_serial;
  };
  for (Retired& retired : retired_) {
    if (reusable(retired)) {
      free_.push_back(std::move(retired.binding));
    }
  }
  std::erase_if(retired_, reusable);
}

MaterialSystem::Binding MaterialSystem::AcquireBinding() {
  if (!free_.empty()) {
    Binding binding = std::move(free_.back());
    free_.pop_back();
    return binding;
  }
  return {.set = AllocateSet(),
          .params = Buffer(context_.allocator(), sizeof(MaterialParams),
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           Buffer::Access::kCpuSequentialWrite)};
}

VkDescriptorSet MaterialSystem::AllocateSet() {
  if (pools_.empty() || sets_in_last_pool_ == kSetsPerPool) {
    pools_.push_back(CreatePool(context_.device()));
    sets_in_last_pool_ = 0;
  }
  const VkDescriptorSetAllocateInfo set_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = pools_.back(),
      .descriptorSetCount = 1,
      .pSetLayouts = &layout_,
  };
  VkDescriptorSet set = VK_NULL_HANDLE;
  CheckVk(vkAllocateDescriptorSets(context_.device(), &set_info, &set));
  ++sets_in_last_pool_;
  return set;
}

void MaterialSystem::Write(const Binding& binding,
                           const material::MaterialDesc& desc) {
  const MaterialParams params = MakeMaterialParams(desc);
  std::memcpy(binding.params.mapped(), &params, sizeof(params));

  const std::array<VkImageView, kTextureCount> views{
      textures_.Get(desc.base_color_texture, TextureKind::kColor,
                    DefaultTexture::kWhite),
      textures_.GetMetallicRoughness(desc),
      textures_.Get(desc.normal_texture, TextureKind::kData,
                    DefaultTexture::kFlatNormal),
      textures_.Get(desc.occlusion_texture, TextureKind::kData,
                    DefaultTexture::kWhite),
      textures_.Get(desc.emissive_texture, TextureKind::kColor,
                    DefaultTexture::kWhite),
  };
  const VkDescriptorBufferInfo buffer_info{
      .buffer = binding.params.handle(),
      .offset = 0,
      .range = sizeof(MaterialParams),
  };
  std::array<VkDescriptorImageInfo, kTextureCount> image_infos{};
  std::array<VkWriteDescriptorSet, 1 + kTextureCount> writes{};
  writes[0] = {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = binding.set,
      .dstBinding = kParamsBinding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .pBufferInfo = &buffer_info,
  };
  for (uint32_t i = 0; i < kTextureCount; ++i) {
    image_infos[i] = {
        .sampler = samplers_.material(),
        .imageView = views[i],
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    writes[i + 1] = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = binding.set,
        .dstBinding = i + 1,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_infos[i],
    };
  }
  vkUpdateDescriptorSets(context_.device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

}  // namespace blue::render
