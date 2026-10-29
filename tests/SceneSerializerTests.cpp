#include "scene/BinaryStream.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace blue;

namespace {

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E', 'S', 'C', 'N', '\0'};
constexpr uint32_t kEnvr = makeFourcc('E', 'N', 'V', 'R');
constexpr uint32_t kEnts = makeFourcc('E', 'N', 'T', 'S');

// Unique per-test folder under the system temp directory, removed on destruction.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<unsigned> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("blue_scene_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
        fs::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::vector<std::byte> readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void writeFile(const fs::path& path, std::span<const std::byte> bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool sameBits(float a, float b)
{
    return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool sameBits(const glm::vec3& a, const glm::vec3& b)
{
    return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
}

bool sameBits(const glm::quat& a, const glm::quat& b)
{
    return sameBits(a.w, b.w) && sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
}

void expectSameScene(const Scene& a, const Scene& b)
{
    CHECK(sameBits(a.camera.position, b.camera.position));
    CHECK(sameBits(a.camera.target, b.camera.target));
    CHECK(sameBits(a.camera.verticalFovDegrees, b.camera.verticalFovDegrees));
    CHECK(sameBits(a.light.position, b.light.position));
    CHECK(sameBits(a.light.color, b.light.color));
    CHECK(sameBits(a.light.intensity, b.light.intensity));
    CHECK(sameBits(a.ambient, b.ambient));

    REQUIRE(a.entities.size() == b.entities.size());
    for (size_t i = 0; i < a.entities.size(); ++i) {
        CHECK(a.entities[i].name == b.entities[i].name);
        CHECK(a.entities[i].meshPath == b.entities[i].meshPath);
        CHECK(sameBits(a.entities[i].transform.position, b.entities[i].transform.position));
        CHECK(sameBits(a.entities[i].transform.rotation, b.entities[i].transform.rotation));
        CHECK(sameBits(a.entities[i].transform.scale, b.entities[i].transform.scale));
    }
}

std::string genericString(const fs::path& path)
{
    const std::u8string u8 = path.lexically_normal().generic_u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

void writeVec3(BinaryWriter& writer, float x, float y, float z)
{
    writer.writeF32(x);
    writer.writeF32(y);
    writer.writeF32(z);
}

// ENVR chunk with default camera/light/ambient values.
void writeEnvironmentChunk(BinaryWriter& writer)
{
    const size_t chunk = writer.beginChunk(kEnvr);
    writeVec3(writer, 2.2f, 1.6f, 2.8f);
    writeVec3(writer, 0.0f, 0.0f, 0.0f);
    writer.writeF32(60.0f);
    writeVec3(writer, 2.5f, 3.5f, 1.5f);
    writeVec3(writer, 1.0f, 0.96f, 0.9f);
    writer.writeF32(18.0f);
    writeVec3(writer, 0.025f, 0.025f, 0.025f);
    writer.endChunk(chunk);
}

std::vector<std::byte> makeFile(std::span<const std::byte> payload, uint32_t version = kSceneFormatVersion)
{
    BinaryWriter file;
    file.writeBytes(std::as_bytes(std::span(kMagic)));
    file.writeU32(version);
    file.writeU32(static_cast<uint32_t>(payload.size()));
    file.writeU32(crc32(payload));
    file.writeBytes(payload);
    return file.bytes();
}

std::vector<std::byte> makeEmptySceneFile(const std::vector<std::byte>& extraChunks = {})
{
    BinaryWriter payload;
    writeEnvironmentChunk(payload);
    payload.writeBytes(extraChunks);
    const size_t entities = payload.beginChunk(kEnts);
    payload.writeU32(0);
    payload.endChunk(entities);
    return makeFile(payload.bytes());
}

Scene makeSampleScene()
{
    Scene scene;
    scene.camera = {{1.5f, 2.5f, -3.5f}, {0.1f, 0.2f, 0.3f}, 72.5f};
    scene.light = {{-4.0f, 5.0f, 6.0f}, {0.3f, 0.6f, 0.9f}, 7.25f};
    scene.ambient = {0.01f, 0.02f, 0.03f};

    scene.entities.push_back({"Cube", "builtin:cube", {}});
    scene.entities.push_back(
        {"Zażółć gęślą jaźń \xF0\x9F\x9A\x80", "models/teapot.obj",
         {{1.0f, -2.0f, 3.5f}, glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f)), {2.0f, 0.5f, 1.25f}}});
    scene.entities.push_back({"", "builtin:plane",
                              {{0.0f, 0.1f, 0.0f}, glm::angleAxis(1.1f, glm::vec3(0.0f, 1.0f, 0.0f)), {10.0f, 1.0f, 10.0f}}});
    return scene;
}

} // namespace

TEST_CASE("round trip preserves every field bit-exactly")
{
    const TempDir dir;
    const fs::path file = dir.path() / "scene.bscn";

    Scene scene = makeSampleScene();
    // Make the relative path absolute so it survives the round trip unchanged.
    scene.entities[1].meshPath = genericString(dir.path() / "models" / "teapot.obj");

    REQUIRE(saveScene(scene, file).has_value());
    const auto loaded = loadScene(file);
    REQUIRE(loaded.has_value());
    expectSameScene(scene, *loaded);
}

TEST_CASE("mesh paths are stored relative to the scene file")
{
    const TempDir dir;
    const fs::path assetFile = dir.path() / "assets" / "teapot.obj";
    const fs::path sceneFile = dir.path() / "scenes" / "a.bscn";

    Scene scene;
    scene.entities.push_back({"Teapot", genericString(assetFile), {}});
    scene.entities.push_back({"Cube", "builtin:cube", {}});

    REQUIRE(saveScene(scene, sceneFile).has_value());

    const std::vector<std::byte> raw = readFile(sceneFile);
    constexpr std::string_view kRelative = "../assets/teapot.obj";
    const auto found = std::ranges::search(raw, std::as_bytes(std::span(kRelative)));
    CHECK_FALSE(found.empty());

    const auto loaded = loadScene(sceneFile);
    REQUIRE(loaded.has_value());
    CHECK(loaded->entities[0].meshPath == genericString(assetFile));
    CHECK(loaded->entities[1].meshPath == "builtin:cube");

    SUBCASE("moving the whole folder keeps the reference valid")
    {
        const fs::path moved = dir.path() / "moved";
        fs::create_directories(moved);
        fs::create_directories(dir.path() / "assets");
        fs::rename(dir.path() / "assets", moved / "assets");
        fs::rename(dir.path() / "scenes", moved / "scenes");

        const auto reloaded = loadScene(moved / "scenes" / "a.bscn");
        REQUIRE(reloaded.has_value());
        CHECK(reloaded->entities[0].meshPath == genericString(moved / "assets" / "teapot.obj"));
    }
}

TEST_CASE("empty scene round trip")
{
    const TempDir dir;
    const fs::path file = dir.path() / "empty.bscn";

    const Scene scene;
    REQUIRE(saveScene(scene, file).has_value());
    const auto loaded = loadScene(file);
    REQUIRE(loaded.has_value());
    CHECK(loaded->entities.empty());
    expectSameScene(scene, *loaded);
}

TEST_CASE("header and payload validation")
{
    const TempDir dir;
    const fs::path file = dir.path() / "bad.bscn";

    SUBCASE("bad magic")
    {
        std::vector<std::byte> bytes = makeEmptySceneFile();
        bytes[0] = static_cast<std::byte>('X');
        writeFile(file, bytes);
        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::BadMagic));
    }

    SUBCASE("newer version")
    {
        BinaryWriter payload;
        writeEnvironmentChunk(payload);
        writeFile(file, makeFile(payload.bytes(), kSceneFormatVersion + 1));
        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::UnsupportedVersion));
    }

    SUBCASE("flipped payload byte fails the checksum")
    {
        std::vector<std::byte> bytes = makeEmptySceneFile();
        bytes.back() ^= std::byte{0x01};
        writeFile(file, bytes);
        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::ChecksumMismatch));
    }

    SUBCASE("unknown chunks are skipped")
    {
        BinaryWriter unknown;
        const size_t chunk = unknown.beginChunk(makeFourcc('X', 'T', 'R', 'A'));
        unknown.writeString("future data");
        unknown.endChunk(chunk);

        writeFile(file, makeEmptySceneFile(unknown.bytes()));
        const auto loaded = loadScene(file);
        REQUIRE(loaded.has_value());
        CHECK(loaded->entities.empty());
    }

    SUBCASE("missing required chunk")
    {
        BinaryWriter payload;
        writeEnvironmentChunk(payload);
        writeFile(file, makeFile(payload.bytes()));
        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::Corrupt));
    }

    SUBCASE("non-finite float")
    {
        BinaryWriter payload;
        const size_t chunk = payload.beginChunk(kEnvr);
        for (int i = 0; i < 17; ++i) {
            payload.writeF32(i == 6 ? std::bit_cast<float>(0x7FC00000U) : 1.0f);
        }
        payload.endChunk(chunk);
        const size_t entities = payload.beginChunk(kEnts);
        payload.writeU32(0);
        payload.endChunk(entities);
        writeFile(file, makeFile(payload.bytes()));
        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::Corrupt));
    }
}

TEST_CASE("every truncated prefix is rejected")
{
    const TempDir dir;
    const fs::path file = dir.path() / "full.bscn";
    const fs::path prefix = dir.path() / "prefix.bscn";

    REQUIRE(saveScene(makeSampleScene(), file).has_value());
    const std::vector<std::byte> bytes = readFile(file);
    REQUIRE(bytes.size() > 20);

    for (size_t length = 0; length < bytes.size(); ++length) {
        writeFile(prefix, std::span(bytes).first(length));
        const auto loaded = loadScene(prefix);
        CHECK_FALSE(loaded.has_value());
    }
}

TEST_CASE("hostile sizes inside a valid checksum are rejected without allocating")
{
    const TempDir dir;
    const fs::path file = dir.path() / "hostile.bscn";

    SUBCASE("entity count")
    {
        BinaryWriter payload;
        writeEnvironmentChunk(payload);
        const size_t entities = payload.beginChunk(kEnts);
        payload.writeU32(1'000'000'000U);
        payload.endChunk(entities);
        writeFile(file, makeFile(payload.bytes()));

        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::Corrupt || loaded.error() == SceneIoError::Truncated));
    }

    SUBCASE("string length")
    {
        BinaryWriter payload;
        writeEnvironmentChunk(payload);
        const size_t entities = payload.beginChunk(kEnts);
        payload.writeU32(1);
        payload.writeU32(1'000'000'000U);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.writeU32(0);
        payload.endChunk(entities);
        writeFile(file, makeFile(payload.bytes()));

        const auto loaded = loadScene(file);
        REQUIRE_FALSE(loaded.has_value());
        CHECK((loaded.error() == SceneIoError::Corrupt || loaded.error() == SceneIoError::Truncated));
    }
}

TEST_CASE("save creates missing directories and load reports missing files")
{
    const TempDir dir;
    const fs::path nested = dir.path() / "a" / "b" / "c" / "scene.bscn";

    REQUIRE(saveScene(Scene{}, nested).has_value());
    CHECK(fs::exists(nested));
    CHECK_FALSE(fs::exists(fs::path(nested) += ".tmp"));

    const auto missing = loadScene(dir.path() / "nope.bscn");
    REQUIRE_FALSE(missing.has_value());
    CHECK((missing.error() == SceneIoError::CannotOpen));
}

TEST_CASE("transform matrix is translate * rotate * scale")
{
    const Transform transform{{1.0f, 2.0f, 3.0f}, glm::angleAxis(0.7f, glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f))),
                              {2.0f, 3.0f, 4.0f}};
    const glm::mat4 expected = glm::translate(glm::mat4(1.0f), transform.position) *
                               glm::mat4_cast(transform.rotation) * glm::scale(glm::mat4(1.0f), transform.scale);
    CHECK(transform.matrix() == expected);
}

TEST_CASE("save rejects scenes the loader would refuse")
{
    const TempDir dir;
    const fs::path file = dir.path() / "invalid.bscn";

    SUBCASE("oversized entity name")
    {
        Scene scene;
        scene.entities.push_back({std::string(4097, 'a'), "builtin:cube", {}});
        const auto saved = saveScene(scene, file);
        REQUIRE_FALSE(saved.has_value());
        CHECK((saved.error() == SceneIoError::InvalidScene));
        CHECK_FALSE(fs::exists(file));
        CHECK_FALSE(fs::exists(fs::path(file) += ".tmp"));
    }

    SUBCASE("non-finite position")
    {
        Scene scene;
        scene.entities.push_back({"Bad", "builtin:cube", {}});
        scene.entities[0].transform.position.x = std::bit_cast<float>(0x7FC00000U);
        const auto saved = saveScene(scene, file);
        REQUIRE_FALSE(saved.has_value());
        CHECK((saved.error() == SceneIoError::InvalidScene));
        CHECK_FALSE(fs::exists(file));
    }
}

TEST_CASE("load rejects oversized files before reading them")
{
    const TempDir dir;
    const fs::path file = dir.path() / "huge.bscn";
    {
        std::ofstream out(file, std::ios::binary);
        out.put('x');
    }
    fs::resize_file(file, 64ULL * 1024 * 1024 + 1);

    const auto loaded = loadScene(file);
    REQUIRE_FALSE(loaded.has_value());
    CHECK((loaded.error() == SceneIoError::Corrupt));
}
