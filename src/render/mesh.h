#ifndef BLUE_RENDER_MESH_H_
#define BLUE_RENDER_MESH_H_

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "asset/mesh_data.h"
#include "render/buffer.h"
#include "render/vulkan_context.h"

namespace blue::render {

inline constexpr VkIndexType kIndexType = VK_INDEX_TYPE_UINT32;
static_assert(sizeof(asset::Index) == sizeof(uint32_t),
              "kIndexType must match Index");

// Returns the vertex buffer binding that matches asset::Vertex.
[[nodiscard]] constexpr std::array<VkVertexInputBindingDescription, 1>
VertexBindings() {
  return {{{.binding = 0,
            .stride = sizeof(asset::Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}}};
}

// Returns the vertex attributes that match asset::Vertex: location 0
// position, 1 normal, 2 uv0, 3 tangent (xyz + handedness in w), 4 uv1.
[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 5>
VertexAttributes() {
  return {{
      {.location = 0,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, position)},
      {.location = 1,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, normal)},
      {.location = 2,
       .binding = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(asset::Vertex, uv0)},
      {.location = 3,
       .binding = 0,
       .format = VK_FORMAT_R32G32B32A32_SFLOAT,
       .offset = offsetof(asset::Vertex, tangent)},
      {.location = 4,
       .binding = 0,
       .format = VK_FORMAT_R32G32_SFLOAT,
       .offset = offsetof(asset::Vertex, uv1)},
  }};
}

// Returns only the position attribute of VertexAttributes(), for depth-only
// passes whose vertex shader reads location 0 alone. Same binding and stride,
// so the pass draws from the same vertex buffers as the main pass but fetches
// 12 instead of 56 bytes per vertex.
[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 1>
PositionOnlyVertexAttributes() {
  return {{VertexAttributes()[0]}};
}

// Returns the position and uv attributes of VertexAttributes() (locations
// 0, 2 and 4), for the depth pre-pass: alpha-masked materials need the uvs.
[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 3>
PositionUvVertexAttributes() {
  return {
      {VertexAttributes()[0], VertexAttributes()[2], VertexAttributes()[4]}};
}

// Mesh resident in device-local memory. Move-only. Not thread-safe.
class Mesh {
 public:
  // Uploads `data` to the GPU and blocks until the upload has finished.
  Mesh(const VulkanContext& context, const asset::MeshData& data);

  Mesh(const Mesh&) = delete;
  Mesh& operator=(const Mesh&) = delete;
  Mesh(Mesh&&) noexcept = default;
  Mesh& operator=(Mesh&&) noexcept = default;

  // Records the bind and indexed draw commands for this mesh into `cmd`.
  void Draw(VkCommandBuffer cmd) const;
  // Binds the vertex and index buffers; DrawRange() then draws from them.
  void Bind(VkCommandBuffer cmd) const;
  // Records an indexed draw of `index_count` indices from `first_index`
  // (e.g. one submesh) of the bound mesh.
  static void DrawRange(VkCommandBuffer cmd, uint32_t first_index,
                        uint32_t index_count) {
    vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0);
  }

  [[nodiscard]] uint32_t index_count() const { return index_count_; }
  // False once the mesh was moved from (Renderer::ReleaseMesh() leaves such
  // a tombstone in its slot): nothing may be drawn from it.
  [[nodiscard]] bool resident() const {
    return vertex_buffer_.handle() != VK_NULL_HANDLE;
  }

 private:
  Buffer vertex_buffer_;
  Buffer index_buffer_;
  uint32_t index_count_ = 0;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MESH_H_
