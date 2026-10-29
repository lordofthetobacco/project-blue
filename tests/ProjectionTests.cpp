#include "camera/Projection.hpp"

#include <doctest/doctest.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/vec4.hpp>

using namespace blue;

namespace {

constexpr float kFov = 1.0471976f; // 60 degrees
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.1f;

glm::vec4 ndc(const glm::mat4& projection, const glm::vec3& viewPoint)
{
    const glm::vec4 clip = projection * glm::vec4(viewPoint, 1.0f);
    return clip / clip.w;
}

} // namespace

TEST_CASE("Reversed projection maps the near plane to depth 1")
{
    const glm::mat4 projection = reversedInfinitePerspective(kFov, kAspect, kNear);
    CHECK(ndc(projection, {0.0f, 0.0f, -kNear}).z == doctest::Approx(1.0f));
}

TEST_CASE("Reversed projection maps far distances close to but above depth 0")
{
    const glm::mat4 projection = reversedInfinitePerspective(kFov, kAspect, kNear);
    const float depth = ndc(projection, {0.0f, 0.0f, -1e6f}).z;
    CHECK(depth > 0.0f);
    CHECK(depth < 1e-6f);
}

TEST_CASE("Reversed projection depth strictly decreases with distance")
{
    const glm::mat4 projection = reversedInfinitePerspective(kFov, kAspect, kNear);
    float previous = ndc(projection, {0.0f, 0.0f, -0.1f}).z;
    for (float distance = 0.2f; distance <= 10000.0f; distance *= 1.1f) {
        const float depth = ndc(projection, {0.0f, 0.0f, -distance}).z;
        CHECK(depth < previous);
        previous = depth;
    }
}

TEST_CASE("Reversed projection matches the standard projection in x and y")
{
    const glm::mat4 reversed = reversedInfinitePerspective(kFov, kAspect, kNear);
    const glm::mat4 standard = glm::perspectiveRH_ZO(kFov, kAspect, kNear, 100.0f);
    const glm::vec3 point(1.5f, -0.7f, -4.0f);
    CHECK(ndc(reversed, point).x == doctest::Approx(ndc(standard, point).x));
    CHECK(ndc(reversed, point).y == doctest::Approx(ndc(standard, point).y));
}
