#include "render/mesh.h"

#include <span>

namespace blue::render {

Mesh::Mesh(const VulkanContext& context,
           std::span<const asset::Vertex> vertices,
           std::span<const asset::Index> indices)
    : vertex_buffer_(CreateGpuBuffer(context, std::as_bytes(vertices),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)),
      index_buffer_(CreateGpuBuffer(context, std::as_bytes(indices),
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT)),
      index_count_(static_cast<uint32_t>(indices.size())) {}

void Mesh::Bind(VkCommandBuffer cmd) const {
  const VkBuffer vertex_buffer = vertex_buffer_.handle();
  const VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
  vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, kIndexType);
}

void Mesh::Draw(VkCommandBuffer cmd) const {
  Bind(cmd);
  DrawRange(cmd, 0, index_count_);
}

}  // namespace blue::render
