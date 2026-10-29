#include "asset/ModelImporter.hpp"

#include <doctest/doctest.h>
#include <glm/geometric.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>

namespace {

constexpr float kEpsilon = 1e-3f;

bool nearlyEqual(const glm::vec3& a, const glm::vec3& b)
{
    return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

} // namespace

TEST_CASE("importModel loads the teapot")
{
    const blue::ImportOptions options{.defaultColor = {0.25f, 0.5f, 0.75f}};
    const auto result = blue::importModel(std::filesystem::path(BLUE_SAMPLES_DIR) / "teapot.obj", options);
    REQUIRE(result.has_value());
    const blue::MeshData& mesh = *result;

    CHECK(mesh.indices.size() == 18960);
    REQUIRE_FALSE(mesh.vertices.empty());

    for (const blue::Vertex& vertex : mesh.vertices) {
        CHECK(std::abs(glm::length(vertex.normal) - 1.0f) <= kEpsilon);
        CHECK(vertex.color == options.defaultColor);
    }
    for (blue::Index index : mesh.indices) {
        CHECK(index < mesh.vertices.size());
    }

    CHECK(nearlyEqual(mesh.bounds.min, {-3.0f, 0.0f, -2.0f}));
    CHECK(nearlyEqual(mesh.bounds.max, {3.434f, 3.15f, 2.0f}));
}

TEST_CASE("importModel reports a missing file")
{
    const auto result = blue::importModel(std::filesystem::temp_directory_path() / "blue_no_such_model.obj");
    REQUIRE_FALSE(result.has_value());
    CHECK((result.error() == blue::ImportError::FileNotFound)); // Parenthesised: doctest would find blue::toString by ADL.
}

TEST_CASE("importModel rejects a garbage file without crashing")
{
    const auto path = std::filesystem::temp_directory_path() / "blue_garbage_model.obj";
    {
        std::ofstream file(path, std::ios::binary);
        file << "\x01\x02\xff\xfe junk \x00\x7f not a model";
    }

    const auto result = blue::importModel(path);
    std::filesystem::remove(path);

    REQUIRE_FALSE(result.has_value());
    CHECK((result.error() == blue::ImportError::ParseFailed || result.error() == blue::ImportError::NoMeshes));
}

TEST_CASE("makeCube is a closed cube with outward counter-clockwise faces")
{
    const blue::MeshData cube = blue::makeCube({1.0f, 0.0f, 0.0f});
    REQUIRE(cube.vertices.size() == 24);
    REQUIRE(cube.indices.size() == 36);

    for (size_t i = 0; i < cube.indices.size(); i += 3) {
        const blue::Vertex& a = cube.vertices[cube.indices[i]];
        const blue::Vertex& b = cube.vertices[cube.indices[i + 1]];
        const blue::Vertex& c = cube.vertices[cube.indices[i + 2]];
        const glm::vec3 geometric = glm::cross(b.position - a.position, c.position - a.position);
        CHECK(glm::dot(geometric, a.normal) > 0.0f);
    }
}

TEST_CASE("importModel handles a non-ASCII directory name")
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / fs::path(u8"modele_\u017c\u00f3\u0142w");
    fs::create_directories(dir);
    const fs::path copy = dir / "teapot.obj";
    fs::copy_file(fs::path(BLUE_SAMPLES_DIR) / "teapot.obj", copy, fs::copy_options::overwrite_existing);

    const auto result = blue::importModel(copy);
    fs::remove_all(dir);

    REQUIRE(result.has_value());
    CHECK(result->indices.size() == 18960);
}
