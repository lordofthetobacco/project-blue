#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace blue {

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

// 32-bit so real models (> 65k vertices) fit.
using Index = uint32_t;

struct Bounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    [[nodiscard]] glm::vec3 size() const { return max - min; }
    [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<Index> indices;
    Bounds bounds;
};

// Unit cube centred at the origin. Each face has its own four vertices so it gets a flat
// normal; triangles are counter-clockwise when seen from outside.
[[nodiscard]] MeshData makeCube(glm::vec3 color);

// Square in the XZ plane at y = 0, centred at the origin, normal +Y, counter-clockwise seen from above.
[[nodiscard]] MeshData makePlane(float size, glm::vec3 color);

} // namespace blue
