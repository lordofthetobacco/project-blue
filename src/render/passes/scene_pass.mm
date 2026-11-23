#include "render/passes/scene_pass.h"

#include "render/encoders.h"
#include "render/frame_globals.h"
#include "render/render_targets.h"

namespace blue::render {

namespace {

// Linear radiance where nothing (not even the sky) is drawn.
constexpr MTLClearColor kClearColor{0.015, 0.018, 0.022, 1.0};

// Opens an encoder on the context's HDR target, cleared or loaded per
// `load_action`, with the scene depth loaded, and sets the viewport, the
// scissor and the frame's globals.
id<MTLRenderCommandEncoder> BeginHdrPass(const FrameContext& ctx,
                                         MTLLoadAction load_action,
                                         const char* label) {
  MTLRenderPassDescriptor* descriptor =
      [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = ctx.scene_hdr.texture();
  color.loadAction = load_action;
  color.storeAction = MTLStoreActionStore;
  color.clearColor = kClearColor;
  MTLRenderPassDepthAttachmentDescriptor* depth = descriptor.depthAttachment;
  depth.texture = ctx.scene_depth.texture();
  depth.loadAction = MTLLoadActionLoad;  // Written by the depth pre-pass.
  depth.storeAction = MTLStoreActionStore;

  const id<MTLRenderCommandEncoder> encoder =
      BeginRenderPass(ctx, descriptor, label);
  SetSceneViewport(encoder, ctx.scene_extent);
  ctx.globals.Bind(encoder, ctx.frame_index, ctx.frame_view);
  return encoder;
}

}  // namespace

void SetSceneViewport(id<MTLRenderCommandEncoder> encoder,
                      core::Extent2D extent) {
  [encoder setViewport:MTLViewport{
                           .originX = 0.0,
                           .originY = 0.0,
                           .width = static_cast<double>(extent.width),
                           .height = static_cast<double>(extent.height),
                           .znear = 0.0,
                           .zfar = 1.0,
                       }];
  [encoder setScissorRect:MTLScissorRect{
                              .x = 0,
                              .y = 0,
                              .width = extent.width,
                              .height = extent.height,
                          }];
}

id<MTLRenderCommandEncoder> BeginScenePass(const FrameContext& ctx) {
  return BeginHdrPass(ctx, MTLLoadActionClear, "scene");
}

id<MTLRenderCommandEncoder> ResumeScenePass(const FrameContext& ctx) {
  return BeginHdrPass(ctx, MTLLoadActionLoad, "scene (resumed)");
}

void EndScenePass(id<MTLRenderCommandEncoder> encoder) {
  [encoder endEncoding];
}

}  // namespace blue::render
