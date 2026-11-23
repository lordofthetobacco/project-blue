#include "render/vulkan/material_system.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include <glm/vec4.hpp>

#include "core/log.h"
#include "render/frame_config.h"
#include "render/binding_layout.h"
#include "render/vulkan/descriptor_layout.h"
#include "render/vulkan/vk_check.h"

namespace blue::render::vulkan {

namespace {

constexpr uint32_t kParamsBinding = BLUE_MATERIAL_PARAMS_BINDING;
// The textures: bindings 1-5, in material::TextureSlot order.
constexpr uint32_t kFirstTextureBinding = BLUE_MATERIAL_BASE_COLOR_BINDING;
constexpr uint32_t kTextureCount = kMaterialTextureCount;
static_assert(kTextureCount == material::kTextureSlotCount,
              "One texture binding per material::TextureSlot");
constexpr uint32_t kSamplerBinding = BLUE_MATERIAL_SAMPLER_BINDING;
constexpr uint32_t kSetsPerPool = 64;

VkDescriptorPool CreatePool(VkDevice device) {
  const std::array<VkDescriptorPoolSize, 3> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = kSetsPerPool},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
       .descriptorCount = kSetsPerPool * kTextureCount},
      {.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = kSetsPerPool},
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

MaterialSystem::MaterialSystem(const VulkanContext& context,
                               TextureCache& textures, const Samplers& samplers)
    : context_(context),
      textures_(textures),
      samplers_(samplers),
      layout_(CreateSetLayout(context.device(), kMaterialBindings)) {}

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
  Write(added.binding, added);
  return static_cast<MaterialId>(materials_.size() - 1);
}

MaterialId MaterialSystem::Create(const material::MaterialDesc& desc,
                                  const MaterialTextures& textures) {
  Material& added = materials_.emplace_back();
  added.desc = material::Sanitize(desc);
  added.textures = textures;
  added.variant = VariantOf(added.desc);
  added.binding = AcquireBinding();
  Write(added.binding, added);
  return static_cast<MaterialId>(materials_.size() - 1);
}

void MaterialSystem::Update(MaterialId id, const material::MaterialDesc& desc) {
  const auto index = static_cast<size_t>(id);
  if (index >= materials_.size()) {
    core::log::Fatal("Update of unknown material {}", index);
  }
  if (materials_[index].released) {
    core::log::Warn("Update of released material {} ignored", index);
    return;
  }
  Material& updated = materials_[index];
  Binding fresh = AcquireBinding();
  updated.desc = material::Sanitize(desc);
  updated.variant = VariantOf(updated.desc);
  Write(fresh, updated);
  // Frames up to the current one may have recorded the old set.
  retired_.push_back(
      {.binding = std::exchange(updated.binding, std::move(fresh)),
       .last_frame = frame_serial_});
}

void MaterialSystem::Release(MaterialId id) {
  const auto index = static_cast<size_t>(id);
  if (id == kDefaultMaterial || index >= materials_.size() ||
      materials_[index].released) {
    return;
  }
  Material& released = materials_[index];
  released.released = true;
  released.textures.reset();
  // Frames up to the current one may have recorded its set.
  retired_.push_back(
      {.binding = std::move(released.binding), .last_frame = frame_serial_});
  released.binding = Binding{};
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

std::array<VkImageView, material::kTextureSlotCount>
MaterialSystem::TextureViews(const Material& material) const {
  const MaterialTextures uploaded =
      material.textures.value_or(MaterialTextures{});
  return {
      textures_.ViewOr(uploaded.base_color, DefaultTexture::kWhite),
      textures_.ViewOr(uploaded.metallic_roughness, DefaultTexture::kWhite),
      textures_.ViewOr(uploaded.normal, DefaultTexture::kFlatNormal),
      textures_.ViewOr(uploaded.occlusion, DefaultTexture::kWhite),
      textures_.ViewOr(uploaded.emissive, DefaultTexture::kWhite),
  };
}

void MaterialSystem::Write(const Binding& binding, const Material& material) {
  const MaterialParams params = MakeMaterialParams(material.desc);
  std::memcpy(binding.params.mapped(), &params, sizeof(params));

  const std::array<VkImageView, kTextureCount> views = TextureViews(material);
  const VkDescriptorBufferInfo buffer_info{
      .buffer = binding.params.handle(),
      .offset = 0,
      .range = sizeof(MaterialParams),
  };
  std::array<VkDescriptorImageInfo, kTextureCount> image_infos{};
  const VkDescriptorImageInfo sampler_info{.sampler = samplers_.material()};
  std::array<VkWriteDescriptorSet, 2 + kTextureCount> writes{};
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
        .imageView = views[i],
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    writes[i + 1] =
        ImageWrite(binding.set, kFirstTextureBinding + i, image_infos[i]);
  }
  writes.back() = SamplerWrite(binding.set, kSamplerBinding, sampler_info);
  vkUpdateDescriptorSets(context_.device(),
                         static_cast<uint32_t>(writes.size()), writes.data(), 0,
                         nullptr);
}

}  // namespace blue::render::vulkan
