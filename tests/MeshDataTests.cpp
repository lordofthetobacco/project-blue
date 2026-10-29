#include "asset/MeshData.hpp"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

using namespace blue;

TEST_CASE("makePlane builds one upward-facing quad")
{
    const glm::vec3 color{0.1f, 0.2f, 0.3f};
    const MeshData plane = makePlane(12.0f, color);

    REQUIRE(plane.vertices.size() == 4);
    REQUIRE(plane.indices.size() == 6);
    CHECK(plane.bounds.min == glm::vec3(-6.0f, 0.0f, -6.0f));
    CHECK(plane.bounds.max == glm::vec3(6.0f, 0.0f, 6.0f));

    for (const Vertex& vertex : plane.vertices) {
        CHECK(vertex.normal == glm::vec3(0.0f, 1.0f, 0.0f));
        CHECK(vertex.color == color);
        CHECK(vertex.position.y == 0.0f);
    }
    for (Index index : plane.indices) {
        CHECK(index < plane.vertices.size());
    }
}

TEST_CASE("makePlane triangles are counter-clockwise seen from above")
{
    const MeshData plane = makePlane(2.0f, glm::vec3(1.0f));

    for (size_t i = 0; i < plane.indices.size(); i += 3) {
        const glm::vec3& a = plane.vertices[plane.indices[i]].position;
        const glm::vec3& b = plane.vertices[plane.indices[i + 1]].position;
        const glm::vec3& c = plane.vertices[plane.indices[i + 2]].position;
        CHECK(glm::cross(b - a, c - a).y > 0.0f);
    }
}
