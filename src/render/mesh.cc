#include "render/mesh.h"

#include <span>

namespace blue::render {

Mesh::Mesh(const VulkanContext& context, const asset::MeshData& data)
    : vertex_buffer_(CreateGpuBuffer(context,
                                     std::as_bytes(std::span(data.vertices)),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)),
      index_buffer_(CreateGpuBuffer(context,
                                    std::as_bytes(std::span(data.indices)),
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT)),
      index_count_(static_cast<uint32_t>(data.indices.size())) {}

void Mesh::Draw(VkCommandBuffer cmd) const {
  const VkBuffer vertex_buffer = vertex_buffer_.handle();
  const VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
  vkCmdBindIndexBuffer(cmd, index_buffer_.handle(), 0, kIndexType);
  vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
}

}  // namespace blue::render
