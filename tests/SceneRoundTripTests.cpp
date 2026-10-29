#include "asset/MeshData.hpp"
#include "asset/ModelImporter.hpp"
#include "scene/Placement.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace blue;

namespace {

constexpr size_t kTeapotTriangles = 6320;

// Removed on destruction so failed checks do not leave files behind.
class ScopedTempDir {
public:
    ScopedTempDir()
        : path_(fs::temp_directory_path() /
                ("blue_roundtrip_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        fs::create_directories(path_);
    }

    ~ScopedTempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST_CASE("an imported, placed model survives a scene save and load")
{
    const fs::path teapotPath = fs::path(BLUE_SAMPLES_DIR) / "teapot.obj";
    const auto imported = importModel(teapotPath);
    REQUIRE(imported.has_value());

    const MeshData plane = makePlane(12.0f, glm::vec3(0.1f));
    Scene scene;
    scene.entities.push_back({.name = "ground", .meshPath = "builtin:plane", .transform = {}});
    scene.entities.push_back({
        .name = "teapot",
        .meshPath = teapotPath.generic_string(),
        .transform = fitOnGround(imported->bounds.min, imported->bounds.max, 2.0f, {1.5f, plane.bounds.min.y, -0.5f}),
    });

    const ScopedTempDir dir;
    const fs::path scenePath = dir.path() / "scenes" / "roundtrip.bscn";
    fs::create_directories(scenePath.parent_path());
    REQUIRE(saveScene(scene, scenePath).has_value());

    const auto loaded = loadScene(scenePath);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entities.size() == scene.entities.size());

    const Entity& teapot = loaded->entities[1];
    CHECK(loaded->entities[0].meshPath == "builtin:plane");
    REQUIRE(fs::exists(fs::path(teapot.meshPath)));

    const auto reimported = importModel(fs::path(teapot.meshPath));
    REQUIRE(reimported.has_value());
    CHECK(reimported->indices.size() / 3 == kTeapotTriangles);

    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const Transform& expected = scene.entities[i].transform;
        const Transform& actual = loaded->entities[i].transform;
        CHECK(actual.position == expected.position);
        CHECK(actual.rotation == expected.rotation);
        CHECK(actual.scale == expected.scale);
    }
}
