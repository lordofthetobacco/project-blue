#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace blue {

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // identity (w, x, y, z)
    glm::vec3 scale{1.0f};

    [[nodiscard]] glm::mat4 matrix() const; // T * R * S
};

struct Entity {
    std::string name;
    std::string meshPath; // filesystem path (absolute or relative to the working directory) or "builtin:<id>"
    Transform transform;
};

struct SceneCamera {
    glm::vec3 position{2.2f, 1.6f, 2.8f};
    glm::vec3 target{0.0f};
    float verticalFovDegrees = 60.0f;
};

struct SceneLight {
    glm::vec3 position{2.5f, 3.5f, 1.5f};
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    float intensity = 18.0f;
};

struct Scene {
    std::vector<Entity> entities;
    SceneCamera camera;
    SceneLight light;
    glm::vec3 ambient{0.025f};
};

inline constexpr std::string_view kBuiltinMeshPrefix = "builtin:";

} // namespace blue
