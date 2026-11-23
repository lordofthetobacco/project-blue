#ifndef BLUE_RENDER_METAL_MESH_H_
#define BLUE_RENDER_METAL_MESH_H_

#include "render/metal/metal_types.h"

#include <cstdint>
#include <span>

#include "asset/mesh_data.h"
#include "render/metal/buffer.h"
#include "render/metal/metal_context.h"
#include "render/metal/recording.h"

namespace blue::render::metal {

inline constexpr MTLIndexType kIndexType = MTLIndexTypeUInt32;
static_assert(sizeof(asset::Index) == sizeof(uint32_t),
              "kIndexType must match Index");

// Returns a new vertex descriptor that matches asset::Vertex, read from the
// buffer bound at kBufferVertices (stride sizeof(asset::Vertex)): kAttributePosition
// float3, kAttributeNormal float3, kAttributeUv0 float2, kAttributeTangent
// float4 (xyz + handedness in w), kAttributeUv1 float2. Every mesh pipeline
// uses it; a vertex function reads only the attributes it declares, so
// depth-only passes fetch no more than they need. Built on each call (it is
// only needed when a pipeline is created), autoreleased.
[[nodiscard]] MTLVertexDescriptor* MeshVertexDescriptor();

// Mesh resident in private GPU memory. Move-only. Not thread-safe.
class Mesh {
 public:
  // Uploads `vertices` and `indices` (borrowed for the call) to the GPU
  // and blocks until the upload has finished.
  Mesh(const MetalContext& context, std::span<const asset::Vertex> vertices,
       std::span<const asset::Index> indices);

  Mesh(const Mesh&) = delete;
  Mesh& operator=(const Mesh&) = delete;
  Mesh(Mesh&&) noexcept = default;
  Mesh& operator=(Mesh&&) noexcept = default;

  // Binds this mesh and draws all of its indices.
  void Draw(const Recording& rec, id<MTL4RenderCommandEncoder> encoder) const;
  // Binds the vertex buffer at kBufferVertices of the argument table;
  // DrawRange() then draws from it. Metal takes the index buffer with each
  // draw, not as bound state.
  void Bind(const Recording& rec) const;
  // Draws `index_count` indices from `first_index` (e.g. one submesh) of
  // this mesh, which must be the one last bound with Bind().
  void DrawRange(id<MTL4RenderCommandEncoder> encoder, uint32_t first_index,
                 uint32_t index_count) const;

  [[nodiscard]] uint32_t index_count() const { return index_count_; }
  // GPU memory of the vertex and index buffers, in bytes.
  [[nodiscard]] uint64_t size_bytes() const {
    return vertex_buffer_.size() + index_buffer_.size();
  }
  // False once the mesh was moved from (Renderer::ReleaseMesh() leaves such
  // a tombstone in its slot): nothing may be drawn from it.
  [[nodiscard]] bool resident() const {
    return vertex_buffer_.handle() != nil;
  }

 private:
  Buffer vertex_buffer_;
  Buffer index_buffer_;
  uint32_t index_count_ = 0;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_MESH_H_
