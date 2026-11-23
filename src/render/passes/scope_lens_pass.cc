#include "render/passes/scope_lens_pass.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/render_targets.h"
#include "render/vk_check.h"

namespace blue::render {

namespace {

// Must match ScopeLensUniforms in shaders/scope_lens.glsl (std140).
struct ScopeLensUniforms {
  glm::mat4 lens;
  glm::vec4 right;          // w: magnification
  glm::vec4 up;             // w: apparent half field
  glm::vec4 axis;           // w: picture half field
  glm::vec4 lens_params;    // distortion, aberration, vignetting, uv scale
  glm::vec4 eye;            // lateral xy, relief error, pupil shadow depth
  glm::vec4 pupils;         // exit, eye, picture rendered, min line pixels
  glm::vec4 reticle_color;  // w: line count
  glm::vec4 glass_tint;
  std::array<glm::vec4, kMaxScopeReticleLines> reticle_lines;
  std::array<glm::vec4, kMaxScopeReticleLines / 4> reticle_half_widths;
};
static_assert(kMaxScopeReticleLines % 4 == 0);
static_assert(offsetof(ScopeLensUniforms, right) == 64);
static_assert(offsetof(ScopeLensUniforms, up) == 80);
static_assert(offsetof(ScopeLensUniforms, axis) == 96);
static_assert(offsetof(ScopeLensUniforms, lens_params) == 112);
static_assert(offsetof(ScopeLensUniforms, eye) == 128);
static_assert(offsetof(ScopeLensUniforms, pupils) == 144);
static_assert(offsetof(ScopeLensUniforms, reticle_color) == 160);
static_assert(offsetof(ScopeLensUniforms, glass_tint) == 176);
static_assert(offsetof(ScopeLensUniforms, reticle_lines) == 192);
static_assert(offsetof(ScopeLensUniforms, reticle_half_widths) ==
              192 + 16 * kMaxScopeReticleLines);

constexpr uint32_t kUniformBinding = 0;
constexpr uint32_t kPictureBinding = 1;

// The glass is generated in the vertex shader from two triangles.
constexpr uint32_t kVertexCount = 6;

ScopeLensUniforms MakeUniforms(const ScopeSight& scope) {
  ScopeLensUniforms uniforms{
      .lens = scope.lens,
      .right = glm::vec4(scope.right, scope.magnification),
      .up = glm::vec4(scope.up, scope.apparent_field),
      .axis = glm::vec4(scope.axis, scope.camera_tangent),
      .lens_params = glm::vec4(
          scope.distortion, scope.chromatic_aberration, scope.vignetting,
          static_cast<float>(std::min(scope.resolution, kMaxScopeResolution)) /
              static_cast<float>(kMaxScopeResolution)),
      .eye = glm::vec4(scope.eye_lateral, scope.eye_relief_error,
                       scope.pupil_shadow_depth),
      .pupils =
          glm::vec4(scope.exit_pupil_radius, scope.eye_pupil_radius,
                    scope.render_picture ? 1.0f : 0.0f, scope.min_line_pixels),
      .reticle_color =
          glm::vec4(scope.reticle_color,
                    static_cast<float>(std::min<uint32_t>(
                        scope.reticle_line_count,
                        static_cast<uint32_t>(kMaxScopeReticleLines)))),
      .glass_tint = glm::vec4(scope.glass_tint, 0.0f),
      .reticle_lines = scope.reticle_lines,
      .reticle_half_widths = {},
  };
  for (size_t i = 0; i < kMaxScopeReticleLines; ++i) {
    uniforms.reticle_half_widths[i / 4][static_cast<glm::length_t>(i % 4)] =
        scope.reticle_half_widths[i];
  }
  return uniforms;
}

}  // namespace

ScopeLensPass::ScopeLensPass(const VulkanContext& context,
                             ShaderPrograms& programs,
                             const std::string& shader_dir,
                             VkDescriptorSetLayout frame_layout)
    : device_(context.device()), frame_layout_(frame_layout) {
  const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
      {.binding = kUniformBinding,
       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
      {.binding = kPictureBinding,
       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = 1,
       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
  }};
  const VkDescriptorSetLayoutCreateInfo layout_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = static_cast<uint32_t>(bindings.size()),
      .pBindings = bindings.data(),
  };
  CheckVk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                      &lens_layout_));

  const auto frames = static_cast<uint32_t>(kFramesInFlight);
  const std::array<VkDescriptorPoolSize, 2> pool_sizes{{
      {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = frames},
      {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
       .descriptorCount = frames},
  }};
  const VkDescriptorPoolCreateInfo pool_info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = frames,
      .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
      .pPoolSizes = pool_sizes.data(),
  };
  CheckVk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool_));

  for (Frame& frame : frames_) {
    frame.uniforms = Buffer(context.allocator(), sizeof(ScopeLensUniforms),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            Buffer::Access::kCpuSequentialWrite);
    const VkDescriptorSetAllocateInfo set_info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool_,
        .descriptorSetCount = 1,
        .pSetLayouts = &lens_layout_,
    };
    CheckVk(vkAllocateDescriptorSets(device_, &set_info, &frame.set));
    const VkDescriptorBufferInfo buffer_info{
        .buffer = frame.uniforms.handle(),
        .offset = 0,
        .range = sizeof(ScopeLensUniforms),
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

  programs.Add(core::JoinPath(shader_dir, "scope_lens.vert"),
               core::JoinPath(shader_dir, "scope_lens.frag"),
               [this](std::span<const uint32_t> vertex,
                      std::span<const uint32_t> fragment) {
                 const std::array<VkDescriptorSetLayout, 2> set_layouts{
                     frame_layout_, lens_layout_};
                 // Blended by the rim's coverage, depth tested against the
                 // scene (equal passes too, for a disc on the model's own
                 // glass) without writing depth, visible from both sides.
                 pipeline_ = std::make_unique<Pipeline>(
                     device_,
                     GraphicsPipelineDesc{
                         .vertex_spirv = vertex,
                         .fragment_spirv = fragment,
                         .set_layouts = set_layouts,
                         .color_format = RenderTargets::kHdrFormat,
                         .depth_format = RenderTargets::kDepthFormat,
                         .cull_mode = VK_CULL_MODE_NONE,
                         .depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL,
                         .depth_write = false,
                         .blend = BlendMode::kAlpha,
                     });
               });
}

ScopeLensPass::~ScopeLensPass() {
  vkDestroyDescriptorPool(device_, pool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, lens_layout_, nullptr);
}

void ScopeLensPass::BindPicture(VkImageView view, VkSampler sampler) {
  const VkDescriptorImageInfo image_info{
      .sampler = sampler,
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  for (const Frame& frame : frames_) {
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.set,
        .dstBinding = kPictureBinding,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
    };
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }
  picture_bound_ = true;
}

void ScopeLensPass::Draw(const FrameContext& ctx) const {
  if (!ctx.view.scope || !picture_bound_) {
    return;
  }
  const Frame& frame = frames_[ctx.frame_index];
  const ScopeLensUniforms uniforms = MakeUniforms(*ctx.view.scope);
  std::memcpy(frame.uniforms.mapped(), &uniforms, sizeof(uniforms));

  vkCmdBindPipeline(ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipeline_->handle());
  const std::array<VkDescriptorSet, 2> sets{ctx.frame_set, frame.set};
  vkCmdBindDescriptorSets(
      ctx.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(), 0,
      static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
  vkCmdDraw(ctx.cmd, kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render
