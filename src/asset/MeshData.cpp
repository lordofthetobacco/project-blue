#include "asset/MeshData.hpp"

#include <glm/geometric.hpp>

#include <array>

namespace blue {

MeshData makeCube(glm::vec3 color)
{
    struct Face {
        glm::vec3 normal;
        glm::vec3 up; // Any direction perpendicular to the normal.
    };
    constexpr std::array<Face, 6> faces = {{
        {{1, 0, 0}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 1, 0}},
        {{0, 0, 1}, {0, 1, 0}},
        {{0, 0, -1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, -1}},
        {{0, -1, 0}, {0, 0, 1}},
    }};

    MeshData mesh;
    mesh.vertices.reserve(faces.size() * 4);
    mesh.indices.reserve(faces.size() * 6);

    for (const Face& face : faces) {
        // right x up == normal, so walking right -> up is counter-clockwise seen from outside.
        const glm::vec3 right = glm::cross(face.up, face.normal);
        const glm::vec3 center = face.normal * 0.5f;
        const glm::vec3 r = right * 0.5f;
        const glm::vec3 u = face.up * 0.5f;

        const auto base = static_cast<Index>(mesh.vertices.size());
        mesh.vertices.push_back({center - r - u, face.normal, color});
        mesh.vertices.push_back({center + r - u, face.normal, color});
        mesh.vertices.push_back({center + r + u, face.normal, color});
        mesh.vertices.push_back({center - r + u, face.normal, color});

        constexpr std::array<Index, 6> quadIndices = {0, 1, 2, 2, 3, 0};
        for (Index offset : quadIndices) {
            mesh.indices.push_back(base + offset);
        }
    }
    mesh.bounds = {.min = glm::vec3(-0.5f), .max = glm::vec3(0.5f)};
    return mesh;
}

MeshData makePlane(float size, glm::vec3 color)
{
    const float h = size * 0.5f;
    constexpr glm::vec3 up{0.0f, 1.0f, 0.0f};

    MeshData mesh;
    mesh.vertices = {
        {{-h, 0.0f, -h}, up, color},
        {{-h, 0.0f, h}, up, color},
        {{h, 0.0f, h}, up, color},
        {{h, 0.0f, -h}, up, color},
    };
    mesh.indices = {0, 1, 2, 2, 3, 0};
    mesh.bounds = {.min = {-h, 0.0f, -h}, .max = {h, 0.0f, h}};
    return mesh;
}

} // namespace blue
