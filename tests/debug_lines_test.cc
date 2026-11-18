#include "render/debug_lines.h"

#include <doctest/doctest.h>

#include <cstddef>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

namespace blue::render {

TEST_CASE("PackLineColor packs RGBA8 with red in the low byte, clamped") {
  CHECK(PackLineColor({1.0f, 0.0f, 0.0f, 1.0f}) == 0xFF0000FFu);
  CHECK(PackLineColor({0.0f, 1.0f, 0.0f, 0.0f}) == 0x0000FF00u);
  CHECK(PackLineColor({2.0f, -1.0f, 0.5f, 1.0f}) == 0xFF8000FFu);
}

TEST_CASE("DebugLines keeps depth-tested and on-top lines apart") {
  DebugLines lines;
  CHECK(lines.empty());
  lines.AddLine({0, 0, 0}, {1, 0, 0}, glm::vec4(1.0f));
  lines.AddLine({0, 0, 0}, {0, 1, 0}, glm::vec4(1.0f), LineDepth::kAlwaysOnTop);
  lines.AddLine({0, 0, 0}, {0, 0, 1}, glm::vec4(1.0f), LineDepth::kAlwaysOnTop);
  CHECK(lines.tested().size() == 2);
  CHECK(lines.on_top().size() == 4);
  CHECK(lines.on_top()[3].position == glm::vec3(0, 0, 1));

  lines.Clear();
  CHECK(lines.empty());
}

TEST_CASE("AddBox draws the 12 transformed edges of the box") {
  DebugLines lines;
  const glm::mat4 transform =
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
  lines.AddBox({.min = glm::vec3(-1.0f), .max = glm::vec3(1.0f)}, transform,
               glm::vec4(1.0f));
  REQUIRE(lines.tested().size() == 24);
  for (size_t i = 0; i < lines.tested().size(); i += 2) {
    const glm::vec3 a = lines.tested()[i].position;
    const glm::vec3 b = lines.tested()[i + 1].position;
    // Every edge is axis-aligned, 2 long and on the translated box.
    CHECK(glm::distance(a, b) == doctest::Approx(2.0f));
    CHECK(a.x >= 9.0f);
    CHECK(a.x <= 11.0f);
  }
}

TEST_CASE("AddCircle stays on the circle in the plane of the normal") {
  DebugLines lines;
  const glm::vec3 center(1.0f, 2.0f, 3.0f);
  lines.AddCircle(center, {0.0f, 0.0f, 2.0f}, 0.5f, glm::vec4(1.0f), 16);
  REQUIRE(lines.tested().size() == 32);
  for (const LineVertex& vertex : lines.tested()) {
    CHECK(glm::distance(vertex.position, center) == doctest::Approx(0.5f));
    CHECK(vertex.position.z == doctest::Approx(3.0f));
  }
  // Closed: the last segment ends where the first began.
  CHECK(glm::distance(lines.tested().front().position,
                      lines.tested().back().position) < 1.0e-5f);
}

TEST_CASE("AddGrid snaps to the spacing and colours major lines") {
  DebugLines lines;
  const glm::vec4 minor(0.5f, 0.5f, 0.5f, 1.0f);
  const glm::vec4 major(1.0f);
  lines.AddGrid({0.4f, 0.0f, -0.3f}, 2.0f, 1.0f, minor, 2, major);
  // 5 lines per direction (-2 .. 2), two vertices each.
  REQUIRE(lines.tested().size() == 20);
  size_t major_vertices = 0;
  for (const LineVertex& vertex : lines.tested()) {
    CHECK(vertex.position.y == 0.0f);
    if (vertex.color == PackLineColor(major)) {
      ++major_vertices;
    }
  }
  // x and z at -2, 0 and 2 are major: 3 lines per direction.
  CHECK(major_vertices == 12);

  lines.Clear();
  lines.AddGrid({}, 1.0f, 0.0f, minor, 1, major);
  CHECK(lines.empty());
}

TEST_CASE("AddWireframe draws three edges per triangle") {
  DebugLines lines;
  const asset::MeshData cube = asset::MakeCube();
  lines.AddWireframe(cube, glm::mat4(1.0f), glm::vec4(1.0f));
  CHECK(lines.tested().size() == cube.indices.size() * 2);

  asset::MeshData broken = cube;
  broken.indices[4] = 1000;
  lines.Clear();
  lines.AddWireframe(broken, glm::mat4(1.0f), glm::vec4(1.0f));
  CHECK(lines.tested().size() == 6);  // Only the first, valid triangle.
}

TEST_CASE("AddAxes and AddMarker add one segment per axis") {
  DebugLines lines;
  lines.AddAxes(glm::mat4(1.0f), 2.0f);
  REQUIRE(lines.tested().size() == 6);
  CHECK(lines.tested()[1].position == glm::vec3(2.0f, 0.0f, 0.0f));
  lines.AddMarker(glm::vec3(1.0f), 0.1f, glm::vec4(1.0f));
  CHECK(lines.on_top().size() == 6);
}

}  // namespace blue::render
