#include "render/frame_globals.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>

#include "camera/frustum.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

static_assert(lighting::kCascadeCount == 4,
              "The cascade vec4s in FrameUniforms hold exactly four values");
static_assert(offsetof(FrameUniforms, view_proj) == 0);
static_assert(offsetof(FrameUniforms, view) == 64);
static_assert(offsetof(FrameUniforms, projection) == 128);
static_assert(offsetof(FrameUniforms, inverse_projection) == 192);
static_assert(offsetof(FrameUniforms, inverse_view_proj) == 256);
static_assert(offsetof(FrameUniforms, camera_position) == 320);
static_assert(offsetof(FrameUniforms, ambient_color) == 336);
static_assert(offsetof(FrameUniforms, sun_direction) == 352);
static_assert(offsetof(FrameUniforms, sun_color) == 368);
static_assert(offsetof(FrameUniforms, cascade_view_projection) == 384);
static_assert(offsetof(FrameUniforms, cascade_split_far) == 640);
static_assert(offsetof(FrameUniforms, cascade_texel_world_size) == 656);
static_assert(offsetof(FrameUniforms, shadow_params) == 672);
static_assert(offsetof(FrameUniforms, screen) == 688);
static_assert(offsetof(FrameUniforms, irradiance_sh) == 704);
static_assert(offsetof(FrameUniforms, environment_params) == 848);
static_assert(offsetof(FrameUniforms, point_light_count) == 864);
static_assert(offsetof(FrameUniforms, point_lights) == 880);
static_assert(sizeof(GpuPointLight) == 32);
static_assert(sizeof(FrameUniforms) == 880 + 32 * kMaxPointLights);

// Shadow lookups move along the surface normal by this many shadow texels.
constexpr float kShadowNormalOffsetTexels = 1.5f;

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

// Returns how strongly `light` lights the point `eye` (its irradiance
// there, up to a constant): the priority used to drop lights beyond
// kMaxPointLights.
float LightImportance(const PointLight& light, const glm::vec3& eye) {
  const glm::vec3 offset = light.position - eye;
  const float brightness =
      light.intensity *
      glm::max(light.color.r, glm::max(light.color.g, light.color.b));
  return brightness / glm::max(glm::dot(offset, offset), 1.0f);
}

// Fills the point-light fields of `uniforms` from `lights`: those
// outside `frustum` are culled, then the kMaxPointLights most important
// for `eye` are kept (in their original order when all fit).
void FillPointLights(std::span<const PointLight> lights,
                     const camera::Frustum& frustum, const glm::vec3& eye,
                     FrameUniforms& uniforms) {
  std::array<float, kMaxPointLights> importance{};
  uint32_t count = 0;
  for (const PointLight& light : lights) {
    if (!(light.intensity > 0.0f)) {
      continue;
    }
    if (light.range > 0.0f) {
      const glm::vec3 reach(light.range);
      if (!frustum.Intersects(
              {.min = light.position - reach, .max = light.position + reach})) {
        continue;
      }
    }
    const float score = LightImportance(light, eye);
    uint32_t slot = count;
    if (count == kMaxPointLights) {
      // Full: replace the least important light if this one beats it.
      const auto weakest = std::ranges::min_element(importance);
      if (*weakest >= score) {
        continue;
      }
      slot = static_cast<uint32_t>(weakest - importance.begin());
    } else {
      ++count;
    }
    importance[slot] = score;
    const float inverse_range_sq =
        light.range > 0.0f ? 1.0f / (light.range * light.range) : 0.0f;
    uniforms.point_lights[slot] = {
        .position_inverse_range_sq =
            glm::vec4(light.position, inverse_range_sq),
        .radiance = glm::vec4(light.color * light.intensity, 0.0f),
    };
  }
  uniforms.point_light_count = glm::uvec4(count, 0, 0, 0);
}

}  // namespace

FrameUniforms MakeFrameUniforms(const SceneView& view,
                                std::span<const lighting::Cascade> cascades,
                                bool shadows, VkExtent2D extent) {
  const glm::mat4 view_proj = view.camera.projection * view.camera.view;
  const glm::vec2 size(static_cast<float>(extent.width),
                       static_cast<float>(extent.height));
  FrameUniforms uniforms{
      .view_proj = view_proj,
      .view = view.camera.view,
      .projection = view.camera.projection,
      .inverse_projection = glm::inverse(view.camera.projection),
      .inverse_view_proj = glm::inverse(view_proj),
      .camera_position = glm::vec4(view.camera.position, 1.0f),
      .ambient_color = glm::vec4(view.ambient, 0.0f),
      .sun_direction = glm::vec4(view.sun.direction, 0.0f),
      .sun_color = glm::vec4(view.sun.color * view.sun.intensity, 0.0f),
      .cascade_view_projection = {},
      .cascade_split_far = glm::vec4(0.0f),
      .cascade_texel_world_size = glm::vec4(0.0f),
      .shadow_params =
          glm::vec4(shadows ? 1.0f : 0.0f, view.cascade_debug ? 1.0f : 0.0f,
                    lighting::kShadowDistance, kShadowNormalOffsetTexels),
      .screen = glm::vec4(size, 1.0f / glm::max(size, glm::vec2(1.0f))),
      .irradiance_sh = {},
      .environment_params = glm::vec4(0.0f),
      .point_light_count = glm::uvec4(0),
      .point_lights = {},
  };
  FillPointLights(view.point_lights, camera::Frustum(view_proj),
                  view.camera.position, uniforms);
  if (shadows) {
    for (size_t i = 0; i < cascades.size() && i < lighting::kCascadeCount;
         ++i) {
      const auto component = static_cast<glm::length_t>(i);
      uniforms.cascade_view_projection[i] = cascades[i].view_projection;
      uniforms.cascade_split_far[component] = cascades[i].split_far;
      uniforms.cascade_texel_world_size[component] =
          cascades[i].texel_world_size;
    }
  }
  return uniforms;
}

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
