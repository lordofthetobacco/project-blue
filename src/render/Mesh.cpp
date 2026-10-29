#include "render/Mesh.hpp"

#include <span>

namespace blue {

Mesh::Mesh(const VulkanContext& context, const MeshData& data)
    : vertexBuffer_(createGpuBuffer(context, std::as_bytes(std::span(data.vertices)), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
    , indexBuffer_(createGpuBuffer(context, std::as_bytes(std::span(data.indices)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
    , indexCount_(static_cast<uint32_t>(data.indices.size()))
{
}

void Mesh::draw(VkCommandBuffer cmd) const
{
    const VkBuffer vertexBuffer = vertexBuffer_.handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_.handle(), 0, kIndexType);
    vkCmdDrawIndexed(cmd, indexCount_, 1, 0, 0, 0);
}

} // namespace blue
