#include "render/mesh.h"

#include <cstddef>
#include <span>

#include "shaders/bindings.h"

namespace blue::render {

namespace {

void SetAttribute(MTLVertexDescriptor* descriptor, VertexAttributeIndex index,
                  MTLVertexFormat format, size_t offset) {
  MTLVertexAttributeDescriptor* attribute = descriptor.attributes[index];
  attribute.format = format;
  attribute.offset = offset;
  attribute.bufferIndex = kBufferVertices;
}

}  // namespace

MTLVertexDescriptor* MeshVertexDescriptor() {
  MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
  SetAttribute(descriptor, kAttributePosition, MTLVertexFormatFloat3,
               offsetof(asset::Vertex, position));
  SetAttribute(descriptor, kAttributeNormal, MTLVertexFormatFloat3,
               offsetof(asset::Vertex, normal));
  SetAttribute(descriptor, kAttributeUv0, MTLVertexFormatFloat2,
               offsetof(asset::Vertex, uv0));
  SetAttribute(descriptor, kAttributeTangent, MTLVertexFormatFloat4,
               offsetof(asset::Vertex, tangent));
  SetAttribute(descriptor, kAttributeUv1, MTLVertexFormatFloat2,
               offsetof(asset::Vertex, uv1));
  MTLVertexBufferLayoutDescriptor* layout =
      descriptor.layouts[kBufferVertices];
  layout.stride = sizeof(asset::Vertex);
  layout.stepFunction = MTLVertexStepFunctionPerVertex;
  layout.stepRate = 1;
  return descriptor;
}

Mesh::Mesh(const MetalContext& context,
           std::span<const asset::Vertex> vertices,
           std::span<const asset::Index> indices)
    : vertex_buffer_(
          CreateGpuBuffer(context, std::as_bytes(vertices), "Mesh vertices")),
      index_buffer_(
          CreateGpuBuffer(context, std::as_bytes(indices), "Mesh indices")),
      index_count_(static_cast<uint32_t>(indices.size())) {}

void Mesh::Bind(id<MTLRenderCommandEncoder> encoder) const {
  [encoder setVertexBuffer:vertex_buffer_.handle()
                    offset:0
                   atIndex:kBufferVertices];
}

void Mesh::DrawRange(id<MTLRenderCommandEncoder> encoder, uint32_t first_index,
                     uint32_t index_count) const {
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:index_count
                       indexType:kIndexType
                     indexBuffer:index_buffer_.handle()
               indexBufferOffset:first_index * sizeof(asset::Index)];
}

void Mesh::Draw(id<MTLRenderCommandEncoder> encoder) const {
  Bind(encoder);
  DrawRange(encoder, 0, index_count_);
}

}  // namespace blue::render
