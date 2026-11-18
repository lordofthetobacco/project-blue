#ifndef BLUE_SCENE_SCENE_H_
#define BLUE_SCENE_SCENE_H_

#include <optional>
#include <string>
#include <string_view>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

// Value types shared by scenes (.mscn, see scene_data.h), physics bodies,
// cameras and the renderer.

namespace blue::scene {

struct Transform {
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity (w, x, y, z)
  glm::vec3 scale{1.0f};

  // Returns the local-to-world matrix, T * R * S.
  [[nodiscard]] glm::mat4 Matrix() const;
};

enum class MotionType {
  kStatic,   // Never moves; other bodies collide with it.
  kDynamic,  // Moved by gravity, forces and collisions.
};

enum class ShapeType {
  kBox,           // Box fitted to the mesh bounds.
  kSphere,        // Sphere fitted to the mesh bounds.
  kConvexHull,    // Convex hull of the mesh vertices.
  kTriangleMesh,  // The mesh triangles; static bodies only.
};

// Passive physics description of a body built from a mesh. Shapes are
// derived from the mesh bounds (box, sphere) or vertices (hull, triangle
// mesh) at body creation and scaled by the body's scale. Units are SI.
struct RigidBody {
  MotionType motion = MotionType::kDynamic;
  ShapeType shape = ShapeType::kBox;
  float mass = 10.0f;        // kg; dynamic bodies only, must be > 0.
  float friction = 0.6f;     // Coulomb coefficient, >= 0.
  float restitution = 0.1f;  // Bounciness in [0, 1].
};

// Returns why `body` is invalid, or std::nullopt if it is valid: every value
// must be finite, a dynamic body needs mass > 0, friction >= 0, restitution
// must lie in [0, 1], and a triangle mesh shape requires a static body. The
// returned view points to a string literal.
[[nodiscard]] std::optional<std::string_view> ValidateRigidBody(
    const RigidBody& body);

struct SceneCamera {
  glm::vec3 position{2.2f, 1.6f, 2.8f};
  glm::vec3 target{0.0f};
  float vertical_fov_degrees = 60.0f;
};

// Directional light (the sun). `direction` is the direction the light
// travels, normalized: (0, -1, 0) for a sun straight overhead.
struct SunLight {
  // normalize(-0.4, -1.0, -0.3); the unnormalized length is sqrt(1.25).
  glm::vec3 direction{-0.357771f, -0.894427f, -0.268328f};
  glm::vec3 color{1.0f, 0.95f, 0.85f};  // Linear RGB.
  float intensity = 3.0f;
  bool casts_shadows = true;
};

// Returns why `sun` is invalid, or std::nullopt if it is valid: every value
// must be finite, the direction must be longer than 1e-6 (it need not be
// normalized), and color and intensity must be >= 0. The returned view points
// to a string literal.
[[nodiscard]] std::optional<std::string_view> ValidateSunLight(
    const SunLight& sun);

// Image-based lighting of the scene: an HDRI environment map drawn as the sky
// and used for ambient light and reflections.
struct Environment {
  // Radiance .hdr equirect panorama (absolute, or relative to the working
  // directory); empty uses the generated default sky
  // (lighting::MakeDefaultEnvironment()).
  std::string hdri_path;
  float intensity = 1.0f;         // scales the environment's radiance, >= 0
  float rotation_radians = 0.0f;  // about +Y
  float exposure_ev = 0.0f;       // camera exposure compensation in EV
};

// Returns why `environment` is invalid, or std::nullopt if it is valid: every
// value must be finite and the intensity >= 0. The returned view points to a
// string literal.
[[nodiscard]] std::optional<std::string_view> ValidateEnvironment(
    const Environment& environment);

}  // namespace blue::scene

#endif  // BLUE_SCENE_SCENE_H_
