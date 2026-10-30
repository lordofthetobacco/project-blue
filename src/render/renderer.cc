#include "render/renderer.h"

#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "lighting/cascades.h"
#include "platform/window.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// Reversed-Z: the projection maps near to 1 and infinity to 0, so clear to 0
// and keep the greater value.
constexpr float kDepthClearValue = 0.0f;
constexpr VkCompareOp kDepthCompareOp = VK_COMPARE_OP_GREATER;

// Shadow pass depth bias (see vkCmdSetDepthBias); tune visually.
constexpr float kShadowDepthBiasConstant = 1.25f;
constexpr float kShadowDepthBiasClamp = 0.0f;
constexpr float kShadowDepthBiasSlope = 1.75f;
// Shadow lookups move along the surface normal by this many shadow texels.
constexpr float kShadowNormalOffsetTexels = 1.5f;
// The cascades use standard depth: the far plane is 1.
constexpr float kShadowDepthClearValue = 1.0f;

// Must match FrameUniforms in shaders/common.glsl (std140).
struct FrameUniforms {
  glm::mat4 view_proj;
  glm::mat4 view;
  glm::vec4 camera_position;
  glm::vec4 light_position;
  glm::vec4 light_color;  // rgb: color, a: intensity
  glm::vec4 ambient_color;
  glm::vec4 sun_direction;  // xyz: direction the light travels
  glm::vec4 sun_color;      // rgb: color * intensity
  std::array<glm::mat4, lighting::kCascadeCount> cascade_view_projection;
  glm::vec4 cascade_split_far;  // View-space far distance per cascade.
  glm::vec4 cascade_texel_world_size;
  // x: shadows enabled, y: cascade debug, z: shadow distance, w: normal
  // offset scale in texels.
  glm::vec4 shadow_params;
};
static_assert(lighting::kCascadeCount == 4,
              "The cascade vec4s in FrameUniforms hold exactly four values");
static_assert(offsetof(FrameUniforms, view_proj) == 0);
static_assert(offsetof(FrameUniforms, view) == 64);
static_assert(offsetof(FrameUniforms, camera_position) == 128);
static_assert(offsetof(FrameUniforms, light_position) == 144);
static_assert(offsetof(FrameUniforms, light_color) == 160);
static_assert(offsetof(FrameUniforms, ambient_color) == 176);
static_assert(offsetof(FrameUniforms, sun_direction) == 192);
static_assert(offsetof(FrameUniforms, sun_color) == 208);
static_assert(offsetof(FrameUniforms, cascade_view_projection) == 224);
static_assert(offsetof(FrameUniforms, cascade_split_far) == 480);
static_assert(offsetof(FrameUniforms, cascade_texel_world_size) == 496);
static_assert(offsetof(FrameUniforms, shadow_params) == 512);
static_assert(sizeof(FrameUniforms) == 528);

// Must match DrawConstants in shaders/mesh.vert.
struct DrawConstants {
  glm::mat4 model;
  glm::mat4 normal_matrix;
};
static_assert(sizeof(DrawConstants) <= 128,
              "Push constants must fit the guaranteed minimum of 128 bytes");

// Must match ShadowConstants in shaders/shadow.vert.
struct ShadowConstants {
  glm::mat4 light_model_view_proj;
};
static_assert(sizeof(ShadowConstants) == 64);

constexpr VkClearColorValue kClearColor{{0.015f, 0.018f, 0.022f, 1.0f}};

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = VertexAttributes();
constexpr auto kShadowVertexAttributes = PositionOnlyVertexAttributes();
constexpr VkPushConstantRange kDrawConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = sizeof(DrawConstants),
};
constexpr VkPushConstantRange kShadowConstantsRange{
    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    .offset = 0,
    .size = sizeof(ShadowConstants),
};

VkDescriptorSetLayout CreateFrameSetLayout(VkDevice device) {
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      {
          .binding = 0,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
          .descriptorCount = 1,
          .stageFlags =
              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
      },
      {
          .binding = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
      },
  }};
  const VkDescriptorSetLayoutCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  CheckVk(vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout));
  return layout;
}

VkDescriptorPool CreateDescriptorPool(VkDevice device) {
  const auto frames = static_cast<uint32_t>(kFramesInFlight);
  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frames},
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = frames},
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

ImageDesc DepthImageDesc(VkExtent2D extent) {
  return {
      .extent = extent,
      .format = Renderer::kDepthFormat,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
      .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
  };
}

ImageDesc ShadowMapDesc() {
  return {
      .extent = {lighting::kShadowMapSize, lighting::kShadowMapSize},
      .format = Renderer::kShadowFormat,
      .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
      .aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
      .array_layers = lighting::kCascadeCount,
  };
}

// Returns a sampler that compares against the stored depth: linear filtering
// makes the hardware average the 2x2 comparison results (PCF), and positions
// outside the map read as depth 1, i.e. lit.
VkSampler CreateShadowSampler(VkDevice device) {
  const VkSamplerCreateInfo create_info{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
      .compareEnable = VK_TRUE,
      .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
      .minLod = 0.0f,
      .maxLod = 0.0f,
      .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
  };
  VkSampler sampler = VK_NULL_HANDLE;
  CheckVk(vkCreateSampler(device, &create_info, nullptr, &sampler));
  return sampler;
}

GraphicsPipelineDesc MeshPipelineDesc(
    std::span<const uint32_t> vertex_spirv,
    std::span<const uint32_t> fragment_spirv,
    const VkDescriptorSetLayout& frame_set_layout, VkFormat color_format) {
  return {
      .vertex_spirv = vertex_spirv,
      .fragment_spirv = fragment_spirv,
      .vertex_bindings = kVertexBindings,
      .vertex_attributes = kVertexAttributes,
      .set_layouts = {&frame_set_layout, 1},
      .push_constants = {&kDrawConstantsRange, 1},
      .color_format = color_format,
      .depth_format = Renderer::kDepthFormat,
      .depth_compare_op = kDepthCompareOp,
  };
}

// Depth-only pipeline of the shadow cascades: no fragment stage, no color.
GraphicsPipelineDesc ShadowPipelineDesc(
    std::span<const uint32_t> vertex_spirv) {
  return {
      .vertex_spirv = vertex_spirv,
      .vertex_bindings = kVertexBindings,
      .vertex_attributes = kShadowVertexAttributes,
      .push_constants = {&kShadowConstantsRange, 1},
      .depth_format = Renderer::kShadowFormat,
      // Open meshes (teapot rim, ground) must cast from both sides.
      .cull_mode = VK_CULL_MODE_NONE,
      // Cascades use standard depth (near 0, far 1), unlike the main camera.
      .depth_compare_op = VK_COMPARE_OP_LESS,
      .depth_clamp = true,
      .depth_bias = true,
  };
}

// Source and destination synchronization scope of an image layout transition.
struct BarrierScope {
  VkPipelineStageFlags2 stage;
  VkAccessFlags2 access;
};

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  BarrierScope src, BarrierScope dst, VkImageLayout old_layout,
                  VkImageLayout new_layout, uint32_t layer_count = 1) {
  const VkImageMemoryBarrier2 barrier{
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src.stage,
      .srcAccessMask = src.access,
      .dstStageMask = dst.stage,
      .dstAccessMask = dst.access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {aspect, 0, 1, 0, layer_count},
  };
  const VkDependencyInfo dependency{
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(cmd, &dependency);
}

constexpr VkPipelineStageFlags2 kDepthTestStages =
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
constexpr BarrierScope kShadowSampledReads{
    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};

}  // namespace

Renderer::Renderer(const platform::Window& window,
                   const std::string& shader_dir,
                   shader::CompileMode shader_mode)
    : window_(window),
      context_(window),
      swapchain_(context_, window.PixelSize(), vsync_),
      depth_(context_, DepthImageDesc(swapchain_.extent())),
      shadow_map_(context_, ShadowMapDesc()),
      shadow_sampler_(CreateShadowSampler(context_.device())),
      frame_set_layout_(CreateFrameSetLayout(context_.device())),
      descriptor_pool_(CreateDescriptorPool(context_.device())),
      shader_programs_(context_.device(), shader_mode),
      gpu_timer_(context_) {
  AddMeshProgram(shader_dir);
  AddShadowProgram(shader_dir);
  CreateFrames();
}

void Renderer::AddMeshProgram(const std::string& shader_dir) {
  shader_programs_.Add(
      core::JoinPath(shader_dir, "mesh.vert"),
      core::JoinPath(shader_dir, "mesh.frag"),
      [this](std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment) {
        mesh_pipeline_ = std::make_unique<Pipeline>(
            context_.device(),
            MeshPipelineDesc(vertex, fragment, frame_set_layout_,
                             swapchain_.format()));
      });
}

void Renderer::AddShadowProgram(const std::string& shader_dir) {
  shader_programs_.Add(core::JoinPath(shader_dir, "shadow.vert"), "",
                       [this](std::span<const uint32_t> vertex,
                              std::span<const uint32_t> /*fragment*/) {
                         shadow_pipeline_ = std::make_unique<Pipeline>(
                             context_.device(), ShadowPipelineDesc(vertex));
                       });
}

Renderer::~Renderer() {
  context_.WaitIdle();

  const VkDevice device = context_.device();
  for (Frame& frame : frames_) {
    vkDestroySemaphore(device, frame.image_acquired, nullptr);
    vkDestroyFence(device, frame.in_flight, nullptr);
    vkDestroyCommandPool(device, frame.command_pool, nullptr);
  }
  vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
  vkDestroyDescriptorSetLayout(device, frame_set_layout_, nullptr);
  vkDestroySampler(device, shadow_sampler_, nullptr);
}

void Renderer::CreateFrames() {
  const VkDevice device = context_.device();

  for (Frame& frame : frames_) {
    const VkCommandPoolCreateInfo pool_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = context_.queue_family(),
    };
    CheckVk(
        vkCreateCommandPool(device, &pool_info, nullptr, &frame.command_pool));

    const VkCommandBufferAllocateInfo alloc_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = frame.command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CheckVk(
        vkAllocateCommandBuffers(device, &alloc_info, &frame.command_buffer));

    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    CheckVk(vkCreateFence(device, &fence_info, nullptr, &frame.in_flight));

    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CheckVk(vkCreateSemaphore(device, &semaphore_info, nullptr,
                              &frame.image_acquired));

    frame.uniforms = Buffer(context_.allocator(), sizeof(FrameUniforms),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            Buffer::Access::kCpuSequentialWrite);

    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &frame_set_layout_,
    };
    CheckVk(vkAllocateDescriptorSets(device, &set_info, &frame.descriptor_set));

    const VkDescriptorBufferInfo buffer_info{
        .buffer = frame.uniforms.handle(),
        .offset = 0,
        .range = sizeof(FrameUniforms),
    };
    // All frames in flight sample the same shadow map.
    const VkDescriptorImageInfo shadow_info{
        .sampler = shadow_sampler_,
        .imageView = shadow_map_.view(),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
    };
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &buffer_info,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &shadow_info,
        },
    }};
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  }
}

MeshId Renderer::UploadMesh(const asset::MeshData& data) {
  meshes_.emplace_back(context_, data);
  return static_cast<MeshId>(meshes_.size() - 1);
}

void Renderer::SetVsync(bool enabled) {
  if (vsync_ != enabled) {
    vsync_ = enabled;
    swapchain_dirty_ = true;
  }
}

bool Renderer::RebuildSwapchain() {
  context_.WaitIdle();
  if (!swapchain_.Recreate(window_.PixelSize(), vsync_)) {
    return false;  // Surface has no area; stay dirty and retry next frame.
  }
  depth_ = Image(context_, DepthImageDesc(swapchain_.extent()));
  swapchain_dirty_ = false;
  return true;
}

void Renderer::DrawFrame(const SceneView& view, ImDrawData* ui) {
  const VkExtent2D window_size = window_.PixelSize();
  if (window_size.width == 0 || window_size.height == 0) {
    return;  // Minimized: nothing to present to.
  }
  if (swapchain_dirty_ && !RebuildSwapchain()) {
    return;
  }

  const Frame& frame = frames_[frame_index_];
  CheckVk(vkWaitForFences(context_.device(), 1, &frame.in_flight, VK_TRUE,
                          UINT64_MAX));
  gpu_timer_.Collect(frame_index_);

  const std::optional<uint32_t> image_index = AcquireImage(frame);
  if (!image_index) {
    return;
  }

  // Reset only once we know work will be submitted, otherwise the next wait
  // deadlocks.
  CheckVk(vkResetFences(context_.device(), 1, &frame.in_flight));

  const bool shadows = view.shadows_enabled && view.sun.casts_shadows;
  Cascades cascades{};
  if (shadows) {
    const VkExtent2D extent = swapchain_.extent();
    const float aspect = static_cast<float>(extent.width) /
                         static_cast<float>(std::max(extent.height, 1u));
    cascades = lighting::FitCascades(
        view.camera.view, view.camera.vertical_fov_radians, aspect,
        view.camera.near_plane, view.sun.direction);
  }

  UpdateUniforms(frame, view, cascades, shadows);
  RecordCommands(frame, *image_index, view, cascades, shadows, ui);
  SubmitAndPresent(frame, *image_index);

  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
}

std::optional<uint32_t> Renderer::AcquireImage(const Frame& frame) {
  uint32_t image_index = 0;
  const VkResult result =
      vkAcquireNextImageKHR(context_.device(), swapchain_.handle(), UINT64_MAX,
                            frame.image_acquired, VK_NULL_HANDLE, &image_index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) {
    swapchain_dirty_ = true;
    return std::nullopt;
  }
  if (result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;  // Still presentable; rebuild next frame.
  } else {
    CheckVk(result);
  }
  return image_index;
}

void Renderer::UpdateUniforms(const Frame& frame, const SceneView& view,
                              const Cascades& cascades, bool shadows) const {
  FrameUniforms uniforms{
      .view_proj = view.camera.projection * view.camera.view,
      .view = view.camera.view,
      .camera_position = glm::vec4(view.camera.position, 1.0f),
      .light_position = glm::vec4(view.light.position, 1.0f),
      .light_color = glm::vec4(view.light.color, view.light.intensity),
      .ambient_color = glm::vec4(view.ambient, 0.0f),
      .sun_direction = glm::vec4(view.sun.direction, 0.0f),
      .sun_color = glm::vec4(view.sun.color * view.sun.intensity, 0.0f),
      .cascade_view_projection = {},
      .cascade_split_far = glm::vec4(0.0f),
      .cascade_texel_world_size = glm::vec4(0.0f),
      .shadow_params =
          glm::vec4(shadows ? 1.0f : 0.0f, view.cascade_debug ? 1.0f : 0.0f,
                    lighting::kShadowDistance, kShadowNormalOffsetTexels),
  };
  if (shadows) {
    for (size_t i = 0; i < cascades.size(); ++i) {
      const auto component = static_cast<glm::length_t>(i);
      uniforms.cascade_view_projection[i] = cascades[i].view_projection;
      uniforms.cascade_split_far[component] = cascades[i].split_far;
      uniforms.cascade_texel_world_size[component] =
          cascades[i].texel_world_size;
    }
  }
  std::memcpy(frame.uniforms.mapped(), &uniforms, sizeof(uniforms));
}

void Renderer::SubmitAndPresent(const Frame& frame, uint32_t image_index) {
  const VkSemaphore render_finished = swapchain_.render_finished(image_index);
  const VkSemaphoreSubmitInfo wait_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = frame.image_acquired,
      .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
  };
  const VkSemaphoreSubmitInfo signal_info{
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = render_finished,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkCommandBufferSubmitInfo cmd_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = frame.command_buffer,
  };
  const VkSubmitInfo2 submit_info{
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1,
      .pWaitSemaphoreInfos = &wait_info,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cmd_info,
      .signalSemaphoreInfoCount = 1,
      .pSignalSemaphoreInfos = &signal_info,
  };
  CheckVk(vkQueueSubmit2(context_.queue(), 1, &submit_info, frame.in_flight));

  const VkSwapchainKHR swapchain = swapchain_.handle();
  const VkPresentInfoKHR present_info{
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &render_finished,
      .swapchainCount = 1,
      .pSwapchains = &swapchain,
      .pImageIndices = &image_index,
  };
  const VkResult present_result =
      vkQueuePresentKHR(context_.queue(), &present_info);
  if (present_result == VK_ERROR_OUT_OF_DATE_KHR ||
      present_result == VK_SUBOPTIMAL_KHR) {
    swapchain_dirty_ = true;
  } else {
    CheckVk(present_result);
  }
}

void Renderer::RecordCommands(const Frame& frame, uint32_t image_index,
                              const SceneView& view, const Cascades& cascades,
                              bool shadows, ImDrawData* ui) {
  const VkCommandBuffer cmd = frame.command_buffer;

  CheckVk(vkResetCommandPool(context_.device(), frame.command_pool, 0));
  const VkCommandBufferBeginInfo begin_info{
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  CheckVk(vkBeginCommandBuffer(cmd, &begin_info));
  gpu_timer_.BeginFrame(cmd, frame_index_);
  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kTotal);

  if (shadows) {
    gpu_timer_.Begin(cmd, frame_index_, GpuScope::kShadow);
    RecordShadowPass(cmd, view, cascades);
    gpu_timer_.End(cmd, frame_index_, GpuScope::kShadow);
  } else {
    InitializeShadowMapLayout(cmd);
  }

  gpu_timer_.Begin(cmd, frame_index_, GpuScope::kMain);
  TransitionToAttachments(cmd, image_index);
  BeginRendering(cmd, image_index);
  DrawScene(cmd, frame, view);
  if (ui != nullptr) {
    ImGui_ImplVulkan_RenderDrawData(ui, cmd);
  }
  vkCmdEndRendering(cmd);

  ImageBarrier(cmd, swapchain_.image(image_index), VK_IMAGE_ASPECT_COLOR_BIT,
               {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
               {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE},
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  gpu_timer_.End(cmd, frame_index_, GpuScope::kMain);

  gpu_timer_.End(cmd, frame_index_, GpuScope::kTotal);
  CheckVk(vkEndCommandBuffer(cmd));
}

void Renderer::InitializeShadowMapLayout(VkCommandBuffer cmd) {
  if (shadow_map_initialized_) {
    return;
  }
  ImageBarrier(cmd, shadow_map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
               {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE},
               kShadowSampledReads, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
               lighting::kCascadeCount);
  shadow_map_initialized_ = true;
}

// There is one shadow map for all frames in flight. That is safe because the
// first barrier below has the previous frame's fragment-shader sampled reads
// as its source scope: on the single queue it orders this pass's depth writes
// after those reads (and, transitively, after the previous pass's writes).
void Renderer::RecordShadowPass(VkCommandBuffer cmd, const SceneView& view,
                                const Cascades& cascades) {
  constexpr uint32_t kMapSize = lighting::kShadowMapSize;

  // Moves all layers at once; their old contents are cleared on load.
  ImageBarrier(
      cmd, shadow_map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT, kShadowSampledReads,
      {kDepthTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      lighting::kCascadeCount);
  shadow_map_initialized_ = true;

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    shadow_pipeline_->handle());
  const VkViewport viewport{
      .x = 0.0f,
      .y = 0.0f,
      .width = static_cast<float>(kMapSize),
      .height = static_cast<float>(kMapSize),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, {kMapSize, kMapSize}};

  for (uint32_t layer = 0; layer < cascades.size(); ++layer) {
    const VkRenderingAttachmentInfo depth_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = shadow_map_.layer_view(layer),
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.depthStencil = {kShadowDepthClearValue, 0}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = scissor,
        .layerCount = 1,
        .pDepthAttachment = &depth_attachment,
    };
    vkCmdBeginRendering(cmd, &rendering_info);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdSetDepthBias(cmd, kShadowDepthBiasConstant, kShadowDepthBiasClamp,
                      kShadowDepthBiasSlope);

    for (const DrawItem& item : view.draws) {
      const ShadowConstants constants{
          .light_model_view_proj =
              cascades[layer].view_projection * item.transform,
      };
      vkCmdPushConstants(cmd, shadow_pipeline_->layout(),
                         kShadowConstantsRange.stageFlags, 0, sizeof(constants),
                         &constants);
      meshes_[static_cast<size_t>(item.mesh)].Draw(cmd);
    }
    vkCmdEndRendering(cmd);
  }

  ImageBarrier(
      cmd, shadow_map_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
      {kDepthTestStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
      kShadowSampledReads, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, lighting::kCascadeCount);
}

void Renderer::TransitionToAttachments(VkCommandBuffer cmd,
                                       uint32_t image_index) const {
  // Previous contents are irrelevant for both attachments: they are cleared on
  // load.
  ImageBarrier(
      cmd, swapchain_.image(image_index), VK_IMAGE_ASPECT_COLOR_BIT,
      {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE},
      {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT},
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  ImageBarrier(cmd, depth_.handle(), VK_IMAGE_ASPECT_DEPTH_BIT,
               {VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
               {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
}

void Renderer::BeginRendering(VkCommandBuffer cmd, uint32_t image_index) const {
  const VkExtent2D extent = swapchain_.extent();

  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = swapchain_.image_view(image_index),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = kClearColor},
  };
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = depth_.view(),
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .clearValue = {.depthStencil = {kDepthClearValue, 0}},
  };
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
      .pDepthAttachment = &depth_attachment,
  };
  vkCmdBeginRendering(cmd, &rendering_info);

  // Negative height flips Y so clip space is Y-up, matching GLM and keeping
  // CCW front faces.
  const VkViewport viewport{
      .x = 0.0f,
      .y = static_cast<float>(extent.height),
      .width = static_cast<float>(extent.width),
      .height = -static_cast<float>(extent.height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void Renderer::DrawScene(VkCommandBuffer cmd, const Frame& frame,
                         const SceneView& view) {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    mesh_pipeline_->handle());
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_->layout(), 0, 1, &frame.descriptor_set,
                          0, nullptr);

  stats_ = {};
  for (const DrawItem& item : view.draws) {
    const Mesh& mesh = meshes_[static_cast<size_t>(item.mesh)];
    const DrawConstants draw{
        .model = item.transform,
        .normal_matrix = glm::transpose(glm::inverse(item.transform)),
    };
    vkCmdPushConstants(cmd, mesh_pipeline_->layout(),
                       kDrawConstantsRange.stageFlags, 0, sizeof(draw), &draw);
    mesh.Draw(cmd);
    ++stats_.draw_calls;
    stats_.triangles += mesh.index_count() / 3;
  }
}

}  // namespace blue::render
