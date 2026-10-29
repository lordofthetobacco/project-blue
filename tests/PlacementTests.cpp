#include "scene/Placement.hpp"

#include <doctest/doctest.h>

#include <cmath>

using namespace blue;

namespace {

constexpr float kEpsilon = 1e-5f;

bool nearlyEqual(float a, float b)
{
    return std::abs(a - b) <= kEpsilon;
}

} // namespace

TEST_CASE("fitOnGround scales the largest extent to the target size")
{
    const Transform t = fitOnGround({-1.0f, 0.0f, -0.5f}, {3.0f, 1.0f, 0.5f}, 2.0f, {0.0f, 0.0f, 0.0f});

    CHECK(nearlyEqual(t.scale.x, 0.5f)); // Largest extent is 4 (x).
    CHECK(t.scale.x == t.scale.y);
    CHECK(t.scale.y == t.scale.z);
}

TEST_CASE("fitOnGround centres an off-centre box on the ground point and rests it on the ground")
{
    const glm::vec3 boundsMin{-3.0f, 1.0f, 2.0f};
    const glm::vec3 boundsMax{1.0f, 3.0f, 4.0f};
    const glm::vec3 ground{5.0f, 0.25f, -2.0f};

    const Transform t = fitOnGround(boundsMin, boundsMax, 2.0f, ground);
    const glm::mat4 matrix = t.matrix();

    const glm::vec3 worldMin = glm::vec3(matrix * glm::vec4(boundsMin, 1.0f));
    const glm::vec3 worldMax = glm::vec3(matrix * glm::vec4(boundsMax, 1.0f));

    CHECK(nearlyEqual((worldMin.x + worldMax.x) * 0.5f, ground.x));
    CHECK(nearlyEqual((worldMin.z + worldMax.z) * 0.5f, ground.z));
    CHECK(nearlyEqual(worldMin.y, ground.y));
    CHECK(nearlyEqual(worldMax.x - worldMin.x, 2.0f));
}

TEST_CASE("fitOnGround keeps scale 1 for a box without extent")
{
    const glm::vec3 point{1.0f, 2.0f, 3.0f};
    const Transform t = fitOnGround(point, point, 2.0f, {0.0f, 0.0f, 0.0f});

    CHECK(t.scale == glm::vec3(1.0f));
    CHECK(std::isfinite(t.position.x));
    CHECK(std::isfinite(t.position.y));
    CHECK(std::isfinite(t.position.z));
    CHECK(nearlyEqual(t.position.y, -2.0f)); // The point itself rests on the ground.
}
