#include "render/d3d12/mesh.h"

#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "asset/mesh_data.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/staging_upload.h"

namespace blue::render::d3d12 {

namespace {

constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 5> kVertexInputLayout = {{
    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
     offsetof(asset::Vertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
     offsetof(asset::Vertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(asset::Vertex, uv0),
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,
     offsetof(asset::Vertex, tangent), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(asset::Vertex, uv1),
     D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
}};

}  // namespace

std::span<const D3D12_INPUT_ELEMENT_DESC> VertexInputLayout() {
  return kVertexInputLayout;
}

Mesh::Mesh(const D3d12Context& context, std::span<const asset::Vertex> vertices,
           std::span<const asset::Index> indices)
    // A buffer cannot be empty; an empty mesh draws nothing anyway.
    : vertex_buffer_(context, std::max<uint64_t>(vertices.size_bytes(), 4),
                     Buffer::Access::kGpuOnly, L"blue mesh vertices"),
      index_buffer_(context, std::max<uint64_t>(indices.size_bytes(), 4),
                    Buffer::Access::kGpuOnly, L"blue mesh indices"),
      index_count_(static_cast<uint32_t>(indices.size())) {
  {
    StagingUploader uploader(context,
                             vertices.size_bytes() + indices.size_bytes());
    uploader.CopyToBuffer(std::as_bytes(vertices), vertex_buffer_.resource(), 0);
    uploader.CopyToBuffer(std::as_bytes(indices), index_buffer_.resource(), 0);
    // Finish() executes the copies; at the end of that ExecuteCommandLists()
    // the buffers decay back to COMMON, from which drawing promotes them.
  }
  vertex_view_ = {.BufferLocation = vertex_buffer_.gpu_address(),
                  .SizeInBytes = static_cast<UINT>(vertices.size_bytes()),
                  .StrideInBytes = sizeof(asset::Vertex)};
  index_view_ = {.BufferLocation = index_buffer_.gpu_address(),
                 .SizeInBytes = static_cast<UINT>(indices.size_bytes()),
                 .Format = kIndexFormat};
}

void Mesh::Bind(ID3D12GraphicsCommandList* cmd) const {
  cmd->IASetVertexBuffers(0, 1, &vertex_view_);
  cmd->IASetIndexBuffer(&index_view_);
}

}  // namespace blue::render::d3d12
