#include "asset/model_importer.h"

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>

#include "core/path.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::asset {
namespace {

constexpr float kEpsilon = 1.0e-3f;

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

}  // namespace

TEST_CASE("ImportModel loads the teapot") {
  const ImportOptions options{.default_color = {0.25f, 0.5f, 0.75f}};
  const auto result =
      ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj"), options);
  REQUIRE(result.ok());
  const MeshData& mesh = result.value();

  CHECK(mesh.indices.size() == 18960);
  REQUIRE_FALSE(mesh.vertices.empty());

  for (const Vertex& vertex : mesh.vertices) {
    CHECK(std::abs(glm::length(vertex.normal) - 1.0f) <= kEpsilon);
    CHECK(vertex.color == options.default_color);
  }
  for (const Index index : mesh.indices) {
    CHECK(index < mesh.vertices.size());
  }

  CHECK(NearlyEqual(mesh.bounds.min, {-3.0f, 0.0f, -2.0f}));
  CHECK(NearlyEqual(mesh.bounds.max, {3.434f, 3.15f, 2.0f}));
}

TEST_CASE("ImportModel reports a missing file") {
  const auto result = ImportModel(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_model.obj"));
  REQUIRE_FALSE(result.ok());
  // Parenthesised: doctest would find blue::asset::ToString by ADL.
  CHECK((result.error() == ImportError::kFileNotFound));
}

TEST_CASE("ImportModel rejects a garbage file without crashing") {
  const testing::ScratchDirectory dir("garbage_model");
  const std::string path = dir.Child("blue_garbage_model.obj");
  // Stops at the first NUL, as the old stream write did.
  const std::string_view garbage = "\x01\x02\xff\xfe junk ";
  REQUIRE(
      platform::WriteFileAtomic(path, std::as_bytes(std::span(garbage))).ok());

  const auto result = ImportModel(path);

  REQUIRE_FALSE(result.ok());
  CHECK((result.error() == ImportError::kParseFailed ||
         result.error() == ImportError::kNoMeshes));
}

TEST_CASE("ImportModel handles a non-ASCII directory name") {
  const testing::ScratchDirectory scratch("importer");
  // "modele_" + z-dot o-acute l-stroke w
  const std::string copy =
      scratch.Child("modele_\xC5\xBC\xC3\xB3\xC5\x82w/teapot.obj");
  const auto teapot = platform::ReadFile(
      core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj"), 16 * 1024 * 1024);
  REQUIRE(teapot.ok());
  REQUIRE(platform::WriteFileAtomic(copy, teapot.value()).ok());

  const auto result = ImportModel(copy);

  REQUIRE(result.ok());
  CHECK(result.value().indices.size() == 18960);
}

}  // namespace blue::asset
