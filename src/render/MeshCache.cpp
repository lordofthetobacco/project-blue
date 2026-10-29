#include "render/MeshCache.hpp"

#include "asset/ModelImporter.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/vec3.hpp>

#include <filesystem>
#include <system_error>

namespace blue {

namespace {

constexpr glm::vec3 kCubeColor{0.42f, 0.45f, 0.30f};  // Olive drab, linear.
constexpr glm::vec3 kPlaneColor{0.10f, 0.13f, 0.10f}; // Dark grey-green.
constexpr float kBuiltinPlaneSize = 12.0f;

[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

// Absolute, normalized, '/'-separated: spellings of one file share a cache entry.
[[nodiscard]] std::string fileKey(std::string_view meshPath)
{
    const std::filesystem::path path = pathFromUtf8(meshPath);
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        normalized = std::filesystem::absolute(path).lexically_normal();
    }
    const std::u8string generic = normalized.generic_u8string();
    return {reinterpret_cast<const char*>(generic.data()), generic.size()};
}

} // namespace

std::optional<CachedMesh> MeshCache::get(std::string_view meshPath)
{
    if (const auto found = byPath_.find(meshPath); found != byPath_.end()) {
        return found->second;
    }

    const bool builtin = meshPath.starts_with(kBuiltinMeshPrefix);
    std::string key = builtin ? std::string(meshPath) : fileKey(meshPath);

    auto entry = byKey_.find(key);
    if (entry == byKey_.end()) {
        std::optional<CachedMesh> loaded = load(meshPath, key);
        entry = byKey_.emplace(std::move(key), loaded).first;
    }
    return byPath_.emplace(std::string(meshPath), entry->second).first->second;
}

std::optional<CachedMesh> MeshCache::load(std::string_view meshPath, const std::string& key)
{
    if (meshPath.starts_with(kBuiltinMeshPrefix)) {
        MeshData data;
        if (meshPath == "builtin:cube") {
            data = makeCube(kCubeColor);
        } else if (meshPath == "builtin:plane") {
            data = makePlane(kBuiltinPlaneSize, kPlaneColor);
        } else {
            log::error("Unknown builtin mesh '{}'", meshPath);
            return std::nullopt;
        }
        return CachedMesh{.id = renderer_.uploadMesh(data), .bounds = data.bounds};
    }

    const auto imported = importModel(pathFromUtf8(key));
    if (!imported) {
        log::error("Failed to import '{}': {}", meshPath, toString(imported.error()));
        return std::nullopt;
    }
    return CachedMesh{.id = renderer_.uploadMesh(*imported), .bounds = imported->bounds};
}

} // namespace blue
