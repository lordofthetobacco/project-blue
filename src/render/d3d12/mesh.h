#ifndef BLUE_RENDER_D3D12_MESH_H_
#define BLUE_RENDER_D3D12_MESH_H_

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "asset/mesh_data.h"
#include "render/d3d12/buffer.h"

namespace blue::render::d3d12 {

class D3d12Context;

inline constexpr DXGI_FORMAT kIndexFormat = DXGI_FORMAT_R32_UINT;
static_assert(sizeof(asset::Index) == sizeof(uint32_t),
              "kIndexFormat must match Index");

// Returns the input layout that matches asset::Vertex and the vertex
// inputs of the shaders (semantics POSITION, NORMAL, TEXCOORD0, TANGENT,
// TEXCOORD1; the Vulkan locations 0-4).
[[nodiscard]] std::span<const D3D12_INPUT_ELEMENT_DESC> VertexInputLayout();

// A mesh resident in default-heap buffers. Move-only. Not thread-safe.
class Mesh {
 public:
  // Uploads `vertices` and `indices` (borrowed for the call) to the GPU and
  // blocks until the upload has finished.
  Mesh(const D3d12Context& context, std::span<const asset::Vertex> vertices,
       std::span<const asset::Index> indices);

  Mesh(Mesh&&) noexcept = default;
  Mesh& operator=(Mesh&&) noexcept = default;
  Mesh(const Mesh&) = delete;
  Mesh& operator=(const Mesh&) = delete;

  // Binds the vertex and index buffers; DrawRange() then draws from them.
  void Bind(ID3D12GraphicsCommandList* cmd) const;
  // Records an indexed draw of `index_count` indices from `first_index`
  // (e.g. one submesh) of the bound mesh.
  static void DrawRange(ID3D12GraphicsCommandList* cmd, uint32_t first_index,
                        uint32_t index_count) {
    cmd->DrawIndexedInstanced(index_count, 1, first_index, 0, 0);
  }

  [[nodiscard]] uint32_t index_count() const { return index_count_; }
  // Device memory of the vertex and index buffers, in bytes.
  [[nodiscard]] uint64_t size_bytes() const {
    return vertex_buffer_.size() + index_buffer_.size();
  }
  // False once the mesh was moved from (a released mesh's tombstone).
  [[nodiscard]] bool resident() const {
    return vertex_buffer_.resource() != nullptr;
  }

 private:
  Buffer vertex_buffer_;
  Buffer index_buffer_;
  D3D12_VERTEX_BUFFER_VIEW vertex_view_{};
  D3D12_INDEX_BUFFER_VIEW index_view_{};
  uint32_t index_count_ = 0;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_MESH_H_
