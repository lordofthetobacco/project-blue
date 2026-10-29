#include "scene/SceneSerializer.hpp"

#include "scene/BinaryStream.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace blue {

namespace {

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E', 'S', 'C', 'N', '\0'};
constexpr size_t kHeaderSize = kMagic.size() + 3 * sizeof(uint32_t);
constexpr size_t kChunkHeaderSize = 2 * sizeof(uint32_t);
constexpr uint32_t kMaxStringLength = 4096;
constexpr std::streamoff kMaxSceneFileSize = 64LL * 1024 * 1024;

constexpr uint32_t kEnvironmentChunk = makeFourcc('E', 'N', 'V', 'R');
constexpr uint32_t kEntitiesChunk = makeFourcc('E', 'N', 'T', 'S');

// Two empty strings, vec3 position, quat, vec3 scale: the smallest possible entity record.
constexpr size_t kMinEntityBytes = 2 * sizeof(uint32_t) + (3 + 4 + 3) * sizeof(float);

std::string pathToString(const std::filesystem::path& path)
{
    const std::u8string u8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

std::filesystem::path pathFromString(const std::string& text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool isBuiltinMesh(const std::string& meshPath)
{
    return meshPath.starts_with(kBuiltinMeshPrefix);
}

std::filesystem::path sceneDirectory(const std::filesystem::path& scenePath)
{
    return std::filesystem::absolute(scenePath).lexically_normal().parent_path();
}

std::string storedMeshPath(const std::string& meshPath, const std::filesystem::path& sceneDir)
{
    if (isBuiltinMesh(meshPath)) {
        return meshPath;
    }
    const std::filesystem::path absolute = std::filesystem::absolute(pathFromString(meshPath)).lexically_normal();
    const std::filesystem::path relative = absolute.lexically_relative(sceneDir);
    // Empty when there is no relative form, e.g. a different drive.
    return pathToString(relative.empty() ? absolute : relative);
}

std::string resolvedMeshPath(const std::string& stored, const std::filesystem::path& sceneDir)
{
    if (isBuiltinMesh(stored)) {
        return stored;
    }
    const std::filesystem::path path = pathFromString(stored);
    return pathToString(path.is_absolute() ? path.lexically_normal() : (sceneDir / path).lexically_normal());
}

void writeVec3(BinaryWriter& writer, const glm::vec3& v)
{
    writer.writeF32(v.x);
    writer.writeF32(v.y);
    writer.writeF32(v.z);
}

void writeQuat(BinaryWriter& writer, const glm::quat& q)
{
    writer.writeF32(q.x);
    writer.writeF32(q.y);
    writer.writeF32(q.z);
    writer.writeF32(q.w);
}

glm::vec3 readVec3(BinaryReader& reader)
{
    const float x = reader.readF32();
    const float y = reader.readF32();
    const float z = reader.readF32();
    return {x, y, z};
}

glm::quat readQuat(BinaryReader& reader)
{
    const float x = reader.readF32();
    const float y = reader.readF32();
    const float z = reader.readF32();
    const float w = reader.readF32();
    return {w, x, y, z};
}

bool isFinite(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool isFinite(const glm::quat& q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

std::expected<void, SceneIoError> toResult(ReadStatus status)
{
    switch (status) {
    case ReadStatus::Ok:
        return {};
    case ReadStatus::Truncated:
        return std::unexpected(SceneIoError::Truncated);
    case ReadStatus::LimitExceeded:
        break;
    }
    return std::unexpected(SceneIoError::Corrupt);
}

bool isEnvironmentFinite(const Scene& scene)
{
    return isFinite(scene.camera.position) && isFinite(scene.camera.target) &&
           std::isfinite(scene.camera.verticalFovDegrees) && isFinite(scene.light.position) &&
           isFinite(scene.light.color) && std::isfinite(scene.light.intensity) && isFinite(scene.ambient);
}

// Rejects scenes the loader would refuse, so a save never produces an unreadable file.
bool isSerializable(const Scene& scene, const std::filesystem::path& sceneDir)
{
    if (!isEnvironmentFinite(scene)) {
        return false;
    }
    return std::ranges::all_of(scene.entities, [&](const Entity& entity) {
        return entity.name.size() <= kMaxStringLength &&
               storedMeshPath(entity.meshPath, sceneDir).size() <= kMaxStringLength &&
               isFinite(entity.transform.position) && isFinite(entity.transform.rotation) &&
               isFinite(entity.transform.scale);
    });
}

std::vector<std::byte> encodePayload(const Scene& scene, const std::filesystem::path& sceneDir)
{
    BinaryWriter writer;

    const size_t environment = writer.beginChunk(kEnvironmentChunk);
    writeVec3(writer, scene.camera.position);
    writeVec3(writer, scene.camera.target);
    writer.writeF32(scene.camera.verticalFovDegrees);
    writeVec3(writer, scene.light.position);
    writeVec3(writer, scene.light.color);
    writer.writeF32(scene.light.intensity);
    writeVec3(writer, scene.ambient);
    writer.endChunk(environment);

    const size_t entities = writer.beginChunk(kEntitiesChunk);
    writer.writeU32(static_cast<uint32_t>(scene.entities.size()));
    for (const Entity& entity : scene.entities) {
        writer.writeString(entity.name);
        writer.writeString(storedMeshPath(entity.meshPath, sceneDir));
        writeVec3(writer, entity.transform.position);
        writeQuat(writer, entity.transform.rotation);
        writeVec3(writer, entity.transform.scale);
    }
    writer.endChunk(entities);

    return writer.bytes();
}

std::expected<void, SceneIoError> decodeEnvironment(std::span<const std::byte> body, Scene& scene)
{
    BinaryReader reader(body);
    scene.camera.position = readVec3(reader);
    scene.camera.target = readVec3(reader);
    scene.camera.verticalFovDegrees = reader.readF32();
    scene.light.position = readVec3(reader);
    scene.light.color = readVec3(reader);
    scene.light.intensity = reader.readF32();
    scene.ambient = readVec3(reader);
    if (auto status = toResult(reader.status()); !status) {
        return status;
    }

    if (!isEnvironmentFinite(scene)) {
        return std::unexpected(SceneIoError::Corrupt);
    }
    return {};
}

std::expected<void, SceneIoError> decodeEntities(std::span<const std::byte> body, const std::filesystem::path& sceneDir,
                                                 Scene& scene)
{
    BinaryReader reader(body);
    const uint32_t count = reader.readU32();
    if (auto status = toResult(reader.status()); !status) {
        return status;
    }
    // Validate before reserving so a hostile count cannot drive an allocation.
    if (count > reader.remaining() / kMinEntityBytes) {
        return std::unexpected(SceneIoError::Corrupt);
    }

    scene.entities.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Entity entity;
        entity.name = reader.readString(kMaxStringLength);
        entity.meshPath = resolvedMeshPath(reader.readString(kMaxStringLength), sceneDir);
        entity.transform.position = readVec3(reader);
        entity.transform.rotation = readQuat(reader);
        entity.transform.scale = readVec3(reader);
        if (auto status = toResult(reader.status()); !status) {
            return status;
        }
        if (!isFinite(entity.transform.position) || !isFinite(entity.transform.rotation) ||
            !isFinite(entity.transform.scale)) {
            return std::unexpected(SceneIoError::Corrupt);
        }
        scene.entities.push_back(std::move(entity));
    }
    return {};
}

std::expected<Scene, SceneIoError> decodeFile(std::span<const std::byte> file, const std::filesystem::path& sceneDir)
{
    if (file.size() < kHeaderSize) {
        return std::unexpected(SceneIoError::Truncated);
    }

    BinaryReader header(file);
    const std::span<const std::byte> magic = header.readBytes(kMagic.size());
    if (!std::equal(magic.begin(), magic.end(), kMagic.begin(),
                    [](std::byte a, char b) { return std::to_integer<int>(a) == static_cast<unsigned char>(b); })) {
        return std::unexpected(SceneIoError::BadMagic);
    }
    const uint32_t version = header.readU32();
    const uint32_t payloadSize = header.readU32();
    const uint32_t payloadCrc = header.readU32();

    if (version == 0) {
        return std::unexpected(SceneIoError::Corrupt);
    }
    if (version > kSceneFormatVersion) {
        return std::unexpected(SceneIoError::UnsupportedVersion);
    }
    if (payloadSize > header.remaining()) {
        return std::unexpected(SceneIoError::Truncated);
    }
    const std::span<const std::byte> payload = header.readBytes(payloadSize);
    if (crc32(payload) != payloadCrc) {
        return std::unexpected(SceneIoError::ChecksumMismatch);
    }

    Scene scene;
    bool hasEnvironment = false;
    bool hasEntities = false;
    BinaryReader chunks(payload);
    while (chunks.remaining() > 0) {
        if (chunks.remaining() < kChunkHeaderSize) {
            return std::unexpected(SceneIoError::Truncated);
        }
        const uint32_t fourcc = chunks.readU32();
        const uint32_t size = chunks.readU32();
        if (size > chunks.remaining()) {
            return std::unexpected(SceneIoError::Truncated);
        }
        const std::span<const std::byte> body = chunks.readBytes(size);

        if (fourcc == kEnvironmentChunk || fourcc == kEntitiesChunk) {
            bool& seen = fourcc == kEnvironmentChunk ? hasEnvironment : hasEntities;
            if (seen) {
                return std::unexpected(SceneIoError::Corrupt);
            }
            seen = true;
            const auto decoded =
                fourcc == kEnvironmentChunk ? decodeEnvironment(body, scene) : decodeEntities(body, sceneDir, scene);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
        }
    }

    if (!hasEnvironment || !hasEntities) {
        return std::unexpected(SceneIoError::Corrupt);
    }
    return scene;
}

std::expected<void, SceneIoError> writeAtomically(std::span<const std::byte> bytes, const std::filesystem::path& path)
{
    std::error_code ec;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::unexpected(SceneIoError::CannotOpen);
        }
    }

    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::unexpected(SceneIoError::CannotOpen);
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            return std::unexpected(SceneIoError::WriteFailed);
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return std::unexpected(SceneIoError::WriteFailed);
    }
    return {};
}

} // namespace

const char* toString(SceneIoError error)
{
    switch (error) {
    case SceneIoError::CannotOpen:
        return "cannot open file";
    case SceneIoError::WriteFailed:
        return "write failed";
    case SceneIoError::BadMagic:
        return "not a scene file (bad magic)";
    case SceneIoError::UnsupportedVersion:
        return "unsupported scene version";
    case SceneIoError::Truncated:
        return "file truncated";
    case SceneIoError::ChecksumMismatch:
        return "checksum mismatch";
    case SceneIoError::Corrupt:
        return "corrupt scene data";
    case SceneIoError::InvalidScene:
        return "scene cannot be serialized (oversized string or non-finite value)";
    }
    return "unknown error";
}

std::expected<void, SceneIoError> saveScene(const Scene& scene, const std::filesystem::path& path)
{
    const std::filesystem::path sceneDir = sceneDirectory(path);
    if (!isSerializable(scene, sceneDir)) {
        return std::unexpected(SceneIoError::InvalidScene);
    }
    const std::vector<std::byte> payload = encodePayload(scene, sceneDir);

    BinaryWriter file;
    file.writeBytes(std::as_bytes(std::span(kMagic)));
    file.writeU32(kSceneFormatVersion);
    file.writeU32(static_cast<uint32_t>(payload.size()));
    file.writeU32(crc32(payload));
    file.writeBytes(payload);

    return writeAtomically(file.bytes(), path);
}

std::expected<Scene, SceneIoError> loadScene(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::unexpected(SceneIoError::CannotOpen);
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::unexpected(SceneIoError::CannotOpen);
    }
    if (size > kMaxSceneFileSize) {
        return std::unexpected(SceneIoError::Corrupt);
    }
    in.seekg(0);

    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::unexpected(SceneIoError::CannotOpen);
    }
    return decodeFile(bytes, sceneDirectory(path));
}

} // namespace blue
