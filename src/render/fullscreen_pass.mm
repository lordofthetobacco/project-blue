#include "render/fullscreen_pass.h"

#include <optional>
#include <string>
#include <utility>

#include "core/log.h"
#include "core/path.h"
#include "render/encoders.h"
#include "render/metal_check.h"
#include "shaders/bindings.h"

namespace blue::render {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr NSUInteger kFullscreenVertexCount = 3;

}  // namespace

FullscreenPass::FullscreenPass(const MetalContext& context,
                               ShaderPrograms& programs,
                               const std::string& shader_dir,
                               const FullscreenPassDesc& desc)
    : desc_(desc), sets_(desc.input_set_count) {
  if (desc.input_count > kMaxInputTextures || desc.input_set_count == 0 ||
      desc.pass_constant_size > kMaxPassConstantBytes ||
      desc.label == nullptr) {
    core::log::Fatal("Invalid full-screen pass '{}'", desc.fragment_path);
  }
  id<MTLDevice> device = context.device();
  programs.Add(
      core::JoinPath(shader_dir, "fullscreen.vert.metal"), desc.fragment_path,
      [this, device](const shader::ShaderLibrary& vertex,
                     const shader::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        const GraphicsPipelineDesc pipeline_desc{
            .vertex = &vertex,
            .fragment = fragment,
            .color_format = desc_.output_format,
            .cull_mode = MTLCullModeNone,
            .depth_write = false,
            .blend = desc_.blend,
            .label = desc_.label,
        };
        auto pipeline = Pipeline::Create(device, pipeline_desc);
        if (!pipeline.ok()) {
          return pipeline.error();
        }
        pipeline_ = std::move(pipeline).value();
        return std::nullopt;
      });
}

void FullscreenPass::SetInputs(
    uint32_t set_index, std::span<const id<MTLTexture> __strong> textures,
    id<MTLSamplerState> sampler) {
  if (set_index >= sets_.size() || textures.size() != desc_.input_count) {
    core::log::Fatal("Bad inputs for full-screen pass '{}'",
                     desc_.fragment_path);
  }
  InputSet& set = sets_[set_index];
  set.textures.assign(textures.begin(), textures.end());
  set.sampler = sampler;
}

MTLRenderPassDescriptor* FullscreenPass::DescribeTarget(
    const FullscreenTarget& target) {
  MTLRenderPassDescriptor* descriptor =
      [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* attachment =
      descriptor.colorAttachments[0];
  attachment.texture = target.texture;
  attachment.level = target.level;
  attachment.slice = target.slice;
  attachment.loadAction = target.load_action;
  attachment.storeAction = MTLStoreActionStore;
  return descriptor;
}

void FullscreenPass::Record(const FrameContext& ctx,
                            const FullscreenTarget& target, uint32_t set_index,
                            std::span<const std::byte> constants) const {
  Draw(BeginRenderPass(ctx, DescribeTarget(target), desc_.label), target,
       set_index, constants);
}

void FullscreenPass::RecordTo(id<MTLCommandBuffer> cmd,
                              const FullscreenTarget& target,
                              uint32_t set_index,
                              std::span<const std::byte> constants) const {
  id<MTLRenderCommandEncoder> encoder =
      [cmd renderCommandEncoderWithDescriptor:DescribeTarget(target)];
  FatalIfNil(encoder, nil, std::string("render encoder ") + desc_.label);
  encoder.label = @(desc_.label);
  Draw(encoder, target, set_index, constants);
}

void FullscreenPass::Draw(id<MTLRenderCommandEncoder> encoder,
                          const FullscreenTarget& target, uint32_t set_index,
                          std::span<const std::byte> constants) const {
  if (set_index >= sets_.size() ||
      sets_[set_index].textures.size() != desc_.input_count ||
      (!constants.empty() && constants.size() != desc_.pass_constant_size)) {
    core::log::Fatal("Bad record of full-screen pass '{}'",
                     desc_.fragment_path);
  }
  // y = 0 is the top row, matching varyings.uv.
  [encoder setViewport:MTLViewport{
                           .originX = 0.0,
                           .originY = 0.0,
                           .width = static_cast<double>(target.extent.width),
                           .height = static_cast<double>(target.extent.height),
                           .znear = 0.0,
                           .zfar = 1.0,
                       }];
  if (target.area.width > 0 && target.area.height > 0) {
    [encoder setScissorRect:target.area];
  }
  pipeline_->Bind(encoder);

  const InputSet& set = sets_[set_index];
  for (NSUInteger i = 0; i < set.textures.size(); ++i) {
    [encoder setFragmentTexture:set.textures[i] atIndex:kTextureInput0 + i];
  }
  if (!set.textures.empty()) {
    [encoder setFragmentSamplerState:set.sampler atIndex:kSamplerInput];
  }
  if (!constants.empty()) {
    [encoder setVertexBytes:constants.data()
                     length:constants.size()
                    atIndex:kBufferPass];
    [encoder setFragmentBytes:constants.data()
                       length:constants.size()
                      atIndex:kBufferPass];
  }
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
              vertexStart:0
              vertexCount:kFullscreenVertexCount];
  [encoder endEncoding];
}

}  // namespace blue::render
