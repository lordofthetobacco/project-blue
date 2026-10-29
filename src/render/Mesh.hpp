#pragma once

#include "asset/MeshData.hpp"
#include "render/Buffer.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace blue {

class VulkanContext;

inline constexpr VkIndexType kIndexType = VK_INDEX_TYPE_UINT32;
static_assert(sizeof(Index) == sizeof(uint32_t), "kIndexType must match Index");

[[nodiscard]] constexpr std::array<VkVertexInputBindingDescription, 1> vertexBindings()
{
    return {{{.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}}};
}

[[nodiscard]] constexpr std::array<VkVertexInputAttributeDescription, 3> vertexAttributes()
{
    return {{
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, position)},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal)},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color)},
    }};
}

// Mesh resident in device-local memory.
class Mesh {
public:
    Mesh(const VulkanContext& context, const MeshData& data);

    void draw(VkCommandBuffer cmd) const;

    [[nodiscard]] uint32_t indexCount() const { return indexCount_; }

private:
    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    uint32_t indexCount_ = 0;
};

} // namespace blue
