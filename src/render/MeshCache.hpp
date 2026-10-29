#pragma once

#include "asset/MeshData.hpp"
#include "render/Renderer.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace blue {

struct CachedMesh {
    MeshId id;
    Bounds bounds;
};

// Resolves Entity::meshPath to an uploaded mesh, importing each source once. Failures are
// remembered so a broken path is reported once rather than re-parsed every frame.
class MeshCache {
public:
    explicit MeshCache(Renderer& renderer)
        : renderer_(renderer)
    {
    }

    [[nodiscard]] std::optional<CachedMesh> get(std::string_view meshPath);

private:
    struct StringHash {
        using is_transparent = void;
        [[nodiscard]] size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
    };

    using Entries = std::unordered_map<std::string, std::optional<CachedMesh>, StringHash, std::equal_to<>>;

    [[nodiscard]] std::optional<CachedMesh> load(std::string_view meshPath, const std::string& key);

    Renderer& renderer_;
    // nullopt marks a failed load.
    Entries byKey_;  // Normalized key: one entry per distinct source.
    Entries byPath_; // Paths exactly as entities spell them: allocation- and filesystem-free once warm.
};

} // namespace blue
