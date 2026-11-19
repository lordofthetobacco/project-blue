#include "render/pipeline.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "core/log.h"
#include "core/result.h"
#include "render/metal_check.h"
#include "shader/shader_library_metal.h"

namespace blue::render {

namespace {

using CreateResult = core::Result<std::unique_ptr<Pipeline>, std::string>;

constexpr const char* kVertexEntryPoint = "VertexMain";
constexpr const char* kFragmentEntryPoint = "FragmentMain";

// Returns "<what>: <error text>" (or just `what` without an error).
std::string ErrorText(const char* what, NSError* error) {
  std::string text = what;
  if (error != nil) {
    text += ": ";
    text += error.localizedDescription.UTF8String;
  }
  return text;
}

// Sets the blend state of `mode` on the single color attachment.
void ApplyBlend(BlendMode mode,
                MTLRenderPipelineColorAttachmentDescriptor* attachment) {
  attachment.blendingEnabled = mode != BlendMode::kReplace;
  attachment.rgbBlendOperation = MTLBlendOperationAdd;
  attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
  attachment.destinationRGBBlendFactor = MTLBlendFactorOne;
  // The attachment's alpha is kept by every blending mode.
  attachment.alphaBlendOperation = MTLBlendOperationAdd;
  attachment.sourceAlphaBlendFactor = MTLBlendFactorZero;
  attachment.destinationAlphaBlendFactor = MTLBlendFactorOne;
  attachment.writeMask = MTLColorWriteMaskAll;
  if (mode == BlendMode::kAlpha) {
    attachment.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    attachment.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  }
}

// Returns the library of `library`, which must have been compiled on
// `device`. A library of another device is a programming error.
id<MTLLibrary> LibraryOn(id<MTLDevice> device,
                         const shader::ShaderLibrary& library) {
  id<MTLLibrary> metal_library = shader::MetalLibrary(library);
  if (metal_library == nil) {
    core::log::Fatal("Cannot create a pipeline from an empty shader library");
  }
  if (metal_library.device != device) {
    core::log::Fatal(
        "A shader library was compiled on another Metal device than the "
        "renderer's ({} vs {})",
        metal_library.device.name.UTF8String, device.name.UTF8String);
  }
  return metal_library;
}

}  // namespace

CreateResult Pipeline::Create(id<MTLDevice> device,
                              const GraphicsPipelineDesc& desc) {
  if (desc.vertex == nullptr) {
    core::log::Fatal("A pipeline needs a vertex shader");
  }
  MTLRenderPipelineDescriptor* descriptor =
      [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = desc.label != nullptr ? @(desc.label) : @"pipeline";

  id<MTLLibrary> vertex_library = LibraryOn(device, *desc.vertex);
  descriptor.vertexFunction =
      [vertex_library newFunctionWithName:@(kVertexEntryPoint)];
  if (descriptor.vertexFunction == nil) {
    return CreateResult::Error(
        std::string("The vertex shader has no function ") + kVertexEntryPoint);
  }
  if (desc.fragment != nullptr) {
    id<MTLLibrary> library = LibraryOn(device, *desc.fragment);
    if (desc.fragment_constants.empty()) {
      descriptor.fragmentFunction =
          [library newFunctionWithName:@(kFragmentEntryPoint)];
      if (descriptor.fragmentFunction == nil) {
        return CreateResult::Error(
            std::string("The fragment shader has no function ") +
            kFragmentEntryPoint);
      }
    } else {
      MTLFunctionConstantValues* values =
          [[MTLFunctionConstantValues alloc] init];
      for (size_t i = 0; i < desc.fragment_constants.size(); ++i) {
        const bool value = desc.fragment_constants[i];
        [values setConstantValue:&value type:MTLDataTypeBool atIndex:i];
      }
      NSError* error = nil;
      descriptor.fragmentFunction =
          [library newFunctionWithName:@(kFragmentEntryPoint)
                        constantValues:values
                                 error:&error];
      if (descriptor.fragmentFunction == nil) {
        return CreateResult::Error(
            ErrorText("Specializing the fragment shader failed", error));
      }
    }
  }

  descriptor.vertexDescriptor = desc.vertex_descriptor;
  descriptor.inputPrimitiveTopology = desc.topology;
  descriptor.rasterSampleCount = 1;
  if (desc.color_format != MTLPixelFormatInvalid) {
    MTLRenderPipelineColorAttachmentDescriptor* attachment =
        descriptor.colorAttachments[0];
    attachment.pixelFormat = desc.color_format;
    ApplyBlend(desc.blend, attachment);
  }
  descriptor.depthAttachmentPixelFormat = desc.depth_format;

  NSError* error = nil;
  // Not `std::make_unique`: the constructor is private.
  std::unique_ptr<Pipeline> pipeline(new Pipeline());
  pipeline->state_ = [device newRenderPipelineStateWithDescriptor:descriptor
                                                            error:&error];
  if (pipeline->state_ == nil) {
    return CreateResult::Error(
        ErrorText("Creating the pipeline failed", error));
  }

  if (desc.depth_format != MTLPixelFormatInvalid) {
    MTLDepthStencilDescriptor* depth = [[MTLDepthStencilDescriptor alloc] init];
    depth.label = descriptor.label;
    depth.depthCompareFunction = desc.depth_compare;
    depth.depthWriteEnabled = desc.depth_write;
    pipeline->depth_stencil_ =
        [device newDepthStencilStateWithDescriptor:depth];
    FatalIfNil(pipeline->depth_stencil_, nil, "newDepthStencilState");
  }
  pipeline->cull_mode_ = desc.cull_mode;
  pipeline->depth_clip_mode_ =
      desc.depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
  pipeline->depth_bias_ = desc.depth_bias;
  return CreateResult::Ok(std::move(pipeline));
}

void Pipeline::Bind(id<MTLRenderCommandEncoder> encoder) const {
  [encoder setRenderPipelineState:state_];
  if (depth_stencil_ != nil) {
    [encoder setDepthStencilState:depth_stencil_];
  }
  [encoder setCullMode:cull_mode_];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setDepthClipMode:depth_clip_mode_];
  if (!depth_bias_) {
    [encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
  }
}

}  // namespace blue::render
