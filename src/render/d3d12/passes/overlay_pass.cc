#include "render/d3d12/passes/overlay_pass.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include "render/binding_layout.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/root_layout.h"
#include "render/d3d12/samplers.h"
#include "render/debug_lines.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Line vertex input: LineVertex (shaders/line.slang).
constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 2> kLineLayout = {{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
     offsetof(LineVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
     0},
    {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(LineVertex, color),
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
}};

// Initial line buffer size; grows to the next power of two as needed.
constexpr uint64_t kInitialLineBufferSize = 64 * 1024;

// Must match OutlineConstants in shaders/selection_outline.slang.
struct OutlineConstants {
  glm::vec4 color;  // display space, alpha = opacity
};

}  // namespace

OverlayPass::OverlayPass(const D3d12Context& context, ShaderPrograms& programs,
                         const RootSignature& scene_root,
                         const RootSignature& fullscreen_root,
                         const FrameGlobals& frame_globals,
                         const Samplers& samplers,
                         const std::vector<Mesh>& meshes)
    : context_(context),
      scene_root_(scene_root),
      params_(SceneRootParams::Of(scene_root.layout(), {})),
      frame_globals_(frame_globals),
      samplers_(samplers),
      meshes_(meshes),
      outline_(context, programs, fullscreen_root,
               ShaderProgram::kSelectionOutline, RenderTargets::kLdrFormat,
               BlendMode::kAlpha) {
  programs.Add(ShaderProgram::kLine, [this](std::span<const std::byte> vertex,
                                            std::span<const std::byte> pixel) {
    const auto make = [&](D3D12_COMPARISON_FUNC compare) {
      GraphicsPipelineDesc desc;
      desc.root_signature = scene_root_.get();
      desc.vertex = vertex;
      desc.pixel = pixel;
      desc.input_layout = kLineLayout;
      desc.rtv_formats[0] = RenderTargets::kLdrFormat;
      desc.rtv_count = 1;
      desc.dsv_format = RenderTargets::kDepthFormat;
      desc.depth_test = true;
      desc.depth_write = false;
      desc.depth_func = compare;
      desc.blend = BlendDesc(BlendMode::kAlpha);
      desc.topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      // The Vulkan backend's lines are strict (rectangular) where the
      // device reports strictLines; aliased lines step differently.
      desc.narrow_lines = context_.narrow_lines_supported();
      desc.name = L"blue line pipeline";
      return CreateGraphicsPipeline(context_.device(), desc);
    };
    // Reversed Z: nearer is greater.
    tested_lines_ = make(D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    top_lines_ = make(D3D12_COMPARISON_FUNC_ALWAYS);
  });
  // Hidden and visible coverage are two programs (define VISIBLE of
  // shaders/depth.slang).
  const auto add_mask = [this, &programs](bool visible) {
    programs.Add(visible ? ShaderProgram::kSelectionMaskVisible
                         : ShaderProgram::kSelectionMaskHidden,
                 [this, visible](std::span<const std::byte> vertex,
                                 std::span<const std::byte> pixel) {
                   GraphicsPipelineDesc desc;
                   desc.root_signature = scene_root_.get();
                   desc.vertex = vertex;
                   desc.pixel = pixel;
                   desc.input_layout = VertexInputLayout();
                   desc.rtv_formats[0] = RenderTargets::kSelectionMaskFormat;
                   desc.rtv_count = 1;
                   desc.dsv_format = RenderTargets::kDepthFormat;
                   desc.depth_test = true;
                   desc.depth_write = false;
                   desc.depth_func = visible
                                         ? D3D12_COMPARISON_FUNC_GREATER_EQUAL
                                         : D3D12_COMPARISON_FUNC_ALWAYS;
                   desc.name = L"blue selection mask pipeline";
                   (visible ? mask_visible_ : mask_hidden_) =
                       CreateGraphicsPipeline(context_.device(), desc);
                 });
  };
  add_mask(/*visible=*/false);
  add_mask(/*visible=*/true);
}

void OverlayPass::Record(const FrameContext& ctx) {
  if (!ctx.plan.overlay) {
    return;
  }
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& ldr = ctx.targets.ldr();
  if (!ctx.view.selected_draws.empty()) {
    RecordSelectionMask(ctx);
  }
  {
    BarrierBatch barriers(cmd);
    ldr.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  if (!ctx.view.lines.empty() || !ctx.view.overlay_lines.empty()) {
    UploadLines(ctx);
    BeginOverlayDraws(ctx, ldr.rtv());
    RecordLines(ctx);
  }
  if (!ctx.view.selected_draws.empty()) {
    const OutlineConstants constants{.color = ctx.view.selection_color};
    const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> inputs = {
        ctx.targets.selection_mask().staging_srv()};
    outline_.Record(cmd, ldr.rtv(), ctx.targets.extent(), inputs,
                    samplers_.nearest_clamp(),
                    std::as_bytes(std::span(&constants, 1)));
    ++ctx.stats.draw_calls;
  }
  BarrierBatch barriers(cmd);
  ldr.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void OverlayPass::BeginOverlayDraws(const FrameContext& ctx,
                                    D3D12_CPU_DESCRIPTOR_HANDLE color) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  {
    BarrierBatch barriers(cmd);
    ctx.targets.depth().Transition(barriers, RenderTargets::kDepthReadState);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.targets.depth().read_only_dsv();
  cmd->OMSetRenderTargets(1, &color, FALSE, &dsv);
  // The scene's viewport, so lines and the mask land on the same pixels as
  // the scene depth.
  SetViewport(cmd, ctx.targets.extent());
  cmd->SetGraphicsRootSignature(scene_root_.get());
  frame_globals_.Bind(cmd, params_, ctx.frame);
}

void OverlayPass::UploadLines(const FrameContext& ctx) {
  const std::span<const LineVertex> tested = ctx.view.lines;
  const std::span<const LineVertex> on_top = ctx.view.overlay_lines;
  const uint64_t needed = tested.size_bytes() + on_top.size_bytes();
  Buffer& buffer = line_buffers_[ctx.frame_index];
  if (buffer.size() < needed) {
    uint64_t size = kInitialLineBufferSize;
    while (size < needed) {
      size *= 2;
    }
    // This frame slot's previous use of the buffer has finished (its fence
    // was waited for before recording), so it can be replaced right away.
    buffer =
        Buffer(context_, size, Buffer::Access::kUpload, L"blue overlay lines");
  }
  auto* bytes = static_cast<std::byte*>(buffer.mapped());
  if (!tested.empty()) {
    std::memcpy(bytes, tested.data(), tested.size_bytes());
  }
  if (!on_top.empty()) {
    std::memcpy(bytes + tested.size_bytes(), on_top.data(),
                on_top.size_bytes());
  }
}

void OverlayPass::RecordLines(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  const Buffer& buffer = line_buffers_[ctx.frame_index];
  const D3D12_VERTEX_BUFFER_VIEW view{
      .BufferLocation = buffer.gpu_address(),
      .SizeInBytes = static_cast<UINT>(buffer.size()),
      .StrideInBytes = sizeof(LineVertex),
  };
  cmd->IASetVertexBuffers(0, 1, &view);
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
  const auto draw = [&](ID3D12PipelineState* pipeline, uint32_t first,
                        uint32_t count) {
    if (count == 0) {
      return;
    }
    cmd->SetPipelineState(pipeline);
    cmd->DrawInstanced(count, 1, first, 0);
    ++ctx.stats.draw_calls;
  };
  const auto tested_count = static_cast<uint32_t>(ctx.view.lines.size());
  draw(tested_lines_.Get(), 0, tested_count);
  draw(top_lines_.Get(), tested_count,
       static_cast<uint32_t>(ctx.view.overlay_lines.size()));
}

void OverlayPass::RecordSelectionMask(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& mask = ctx.targets.selection_mask();
  {
    BarrierBatch barriers(cmd);
    mask.Transition(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  cmd->ClearRenderTargetView(mask.rtv(), RenderTargets::kNotSelected, 0,
                             nullptr);
  BeginOverlayDraws(ctx, mask.rtv());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  // Hidden parts first (0.5), then the visible ones overwrite them (1).
  for (ID3D12PipelineState* pipeline :
       {mask_hidden_.Get(), mask_visible_.Get()}) {
    cmd->SetPipelineState(pipeline);
    for (const uint32_t index : ctx.view.selected_draws) {
      if (index >= ctx.view.draws.size()) {
        continue;
      }
      const DrawItem& item = ctx.view.draws[index];
      const auto mesh_index = static_cast<size_t>(item.mesh);
      if (mesh_index >= meshes_.size() || !meshes_[mesh_index].resident()) {
        continue;
      }
      const Mesh& mesh = meshes_[mesh_index];
      const glm::mat4 model = item.transform;
      cmd->SetGraphicsRoot32BitConstants(kPushConstantsParam, sizeof(model) / 4,
                                         &model, 0);
      mesh.Bind(cmd);
      Mesh::DrawRange(cmd, 0, mesh.index_count());
      ++ctx.stats.draw_calls;
    }
  }
  BarrierBatch barriers(cmd);
  mask.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace blue::render::d3d12
