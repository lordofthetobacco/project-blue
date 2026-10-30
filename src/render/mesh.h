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

// Returns the vertex attributes that match asset::Vertex (position, normal,
// color).
[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 3>
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
       .format = VK_FORMAT_R32G32B32_SFLOAT,
       .offset = offsetof(asset::Vertex, color)},
  }};
}

// Returns only the position attribute of VertexAttributes(), for depth-only
// passes whose vertex shader reads location 0 alone. Same binding and stride,
// so the pass draws from the same vertex buffers as the main pass but fetches
// 12 instead of 36 bytes per vertex.
[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 1>
PositionOnlyVertexAttributes() {
  return {{VertexAttributes()[0]}};
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

  [[nodiscard]] uint32_t index_count() const { return index_count_; }

 private:
  Buffer vertex_buffer_;
  Buffer index_buffer_;
  uint32_t index_count_ = 0;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_MESH_H_
