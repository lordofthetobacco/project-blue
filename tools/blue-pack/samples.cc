#include "tools/blue-pack/samples.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/clip_tools.h"
#include "cook/collision_cook.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "core/log.h"
#include "core/path.h"
#include "material/material.h"
#include "material/material_file.h"
#include "physics/physics_runtime.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/default_lighting.h"
#include "scene/mscn_serializer.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "weapon/weapon_geometry.h"

namespace blue::pack {
namespace {

using Clock = std::chrono::steady_clock;

// Body settings of every sample body; the values the code-built default
// scene used (scene::RigidBody's defaults).
constexpr float kFriction = 0.6f;
constexpr float kRestitution = 0.1f;
constexpr float kCrateMass = 10.0f;  // kg

// ---------------------------------------------------------------------------
// Weapons: the calibrations of weapon::M16Geometry() / Glock17Geometry()
// (model units, see weapon_geometry.h for how they were measured); the
// prefabs are in metres, so every point is multiplied by the scale.

// The pistol's firing animation: authored as one clip per node, merged into
// one; the round leaves at "shot" and the slide locks back at "slide_lock".
constexpr std::string_view kPistolFireClip = "fire";
constexpr float kPistolShotTime = 0.333f;     // s
constexpr float kPistolSlideLockTime = 0.4f;  // s
// A static copy of the moving parts merged into one mesh; drawing it would
// hide the slide and trigger and z-fight with them.
constexpr const char* kPistolDuplicateNode = "Glock_parts_ORIGINAL_combined";

// ---------------------------------------------------------------------------
// The range scene (formerly App::InitializeScene).

// Eye position and view of the player at the firing line, looking down range
// (-Z).
constexpr glm::vec3 kFiringPosition{2.2f, 1.6f, 2.8f};
constexpr glm::vec3 kDownRange{0.0f, 0.0f, -1.0f};
constexpr glm::vec3 kRight{1.0f, 0.0f, 0.0f};  // cross(kDownRange, +Y)

// Ground strip: 48 m wide, from 24 m behind the spawn to 700 m down range.
constexpr float kGroundWidth = 48.0f;
constexpr float kGroundBackZ = 24.0f;
constexpr float kGroundFrontZ = -700.0f;
// Ground037 repeats once every 2 m.
constexpr glm::vec2 kGroundUvScale{0.5f};

constexpr glm::vec3 kHousePosition{-9.0f, 0.0f, -3.0f};

// Steel plates: 0.5 m squares up close, 0.5 x 1.0 m torso plates farther out,
// each a little to one side so none hides another from the firing line.
constexpr glm::vec3 kPlateSize{0.5f, 1.0f, 0.05f};  // target.pbo
struct RangeTarget {
  float range;   // m down range from the firing position
  float offset;  // m to the right of the firing line
  float height;  // m, plate height (0.5 = half a torso plate)
};
constexpr RangeTarget kRangeTargets[] = {
    {25.0f, -1.5f, 0.5f}, {50.0f, 1.5f, 0.5f},   {100.0f, -2.5f, 1.0f},
    {200.0f, 2.0f, 1.0f}, {300.0f, -5.0f, 1.0f}, {400.0f, 6.0f, 1.0f},
    {600.0f, 0.0f, 1.0f},
};
constexpr float kTargetCentreHeight = 1.5f;  // m above the ground
// Instances whose name starts with this are targets for the game's hit log.
constexpr std::string_view kTargetNamePrefix = "target ";

// Preview spheres: a dielectric and a metal row, roughness growing away from
// the spawn, each with its material from materials.pbo.
constexpr std::string_view kPreviewKinds[] = {"dielectric", "metal"};
constexpr std::string_view kPreviewRoughnessTags[] = {"005", "025", "050",
                                                      "075", "100"};
constexpr glm::vec3 kPreviewFirstSphere{6.0f, 0.5f, -4.0f};
constexpr float kPreviewSpacing = 1.5f;
constexpr float kPreviewRowSpacing = 2.0f;
constexpr int kSphereSegments = 64;  // as the renderer's builtin sphere

// Unit crate dropped from 3 m at a tilt.
constexpr glm::vec3 kCratePosition{0.0f, 3.0f, 0.0f};
constexpr float kCrateTiltDegrees = 30.0f;

// The point light of the code-built scene; the old renderer light had no
// range, 20 m covers everything it visibly lit.
constexpr glm::vec3 kLampPosition{2.5f, 3.5f, 1.5f};
constexpr glm::vec3 kLampColor{1.0f, 0.96f, 0.9f};
constexpr float kLampRange = 20.0f;

constexpr const char* kHdri = "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr";

// Returns the rotation that turns +Z into `axis` (unit).
glm::quat RotationFromZ(glm::vec3 axis) {
  const glm::vec3 z(0.0f, 0.0f, 1.0f);
  const float cosine = glm::dot(z, axis);
  if (cosine > 0.999999f) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  if (cosine < -0.999999f) {
    return glm::angleAxis(glm::pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
  }
  return glm::angleAxis(std::acos(cosine), glm::normalize(glm::cross(z, axis)));
}

// Adds the sockets of `weapon` to `prefab` (root node, metres).
void AddWeaponSockets(prefab::Prefab& prefab,
                      const weapon::WeaponGeometry& weapon) {
  const float s = weapon.scale;
  prefab.sockets.push_back(
      {.name = "muzzle", .local = {.translation = weapon.muzzle * s}});
  prefab.sockets.push_back({.name = "sight",
                            .local = {.translation = weapon.sight_eye_point * s,
                                      .rotation = RotationFromZ(
                                          glm::normalize(weapon.sight_axis))}});
  prefab.sockets.push_back(
      {.name = "grip", .local = {.translation = weapon.grip * s}});
  if (weapon.optic_lens_radius > 0.0f) {
    prefab.sockets.push_back(
        {.name = "lens",
         .local = {.translation = weapon.optic_lens * s,
                   .scale = glm::vec3(weapon.optic_lens_radius * s)}});
  }
}

// Shared state of one CookSamples() run.
class SampleCooker {
 public:
  explicit SampleCooker(const SamplesOptions& options)
      : options_(options),
        cook_options_{.texture_quality = options.fast
                                             ? cook::CookQuality::kFast
                                             : cook::CookQuality::kHigh} {}

  std::string Source(std::string_view relative) const {
    return core::JoinPath(options_.source_dir, relative);
  }
  std::string Out(std::string_view relative) const {
    return core::JoinPath(options_.out_dir, relative);
  }
  const cook::ModelCookOptions& cook_options() const { return cook_options_; }

  // Saves `prefab` as out/<name>.pbo; logs failures.
  bool Save(prefab::Prefab& prefab, std::string_view name,
            Clock::time_point start) {
    prefab.bounds = cook::ComputePrefabBounds(prefab);
    const std::string path = Out(std::format("{}.pbo", name));
    if (auto saved = prefab::SavePrefab(prefab, path); !saved.ok()) {
      core::log::Error("blue-pack samples: cannot write {}: {}", path,
                       prefab::ToString(saved.error()));
      return false;
    }
    std::vector<std::string> names;
    for (const prefab::Material& material : prefab.materials) {
      names.push_back(material.name);
    }
    material_names_.emplace_back(path, std::move(names));
    core::log::Info(
        "sample {}.pbo built in {:.0f} ms", name,
        std::chrono::duration<double, std::milli>(Clock::now() - start)
            .count());
    return true;
  }

  // Loads a .bmat from the samples directory.
  std::optional<material::MaterialDesc> Material(std::string_view relative) {
    const std::string path = Source(relative);
    auto loaded = material::LoadMaterialFile(path);
    if (!loaded.ok()) {
      core::log::Error("blue-pack samples: cannot load {}: {}", path,
                       material::ToString(loaded.error()));
      return std::nullopt;
    }
    return std::move(loaded).value();
  }

  // Appends `desc` to `prefab`; logs failures.
  bool AddMaterial(prefab::Prefab& prefab, const material::MaterialDesc& desc) {
    auto added = cook::AddMaterialToPrefab(prefab, desc, cook_options_);
    if (!added.ok()) {
      core::log::Error("blue-pack samples: material '{}': {}", desc.name,
                       cook::ToString(added.error()));
    }
    return added.ok();
  }

  // Cooks the collider of `prefab`; logs failures.
  static bool Collider(prefab::Prefab& prefab, prefab::ColliderKind kind,
                       bool dynamic = false) {
    prefab.bounds = cook::ComputePrefabBounds(prefab);
    auto cooked = cook::CookCollision(prefab, {.kind = kind,
                                               .dynamic = dynamic,
                                               .mass = kCrateMass,
                                               .friction = kFriction,
                                               .restitution = kRestitution});
    if (!cooked.ok()) {
      core::log::Error("blue-pack samples: collider: {}",
                       cook::ToString(cooked.error()));
    }
    return cooked.ok();
  }

  // Material names per written prefab path, for the scene check.
  const std::vector<std::pair<std::string, std::vector<std::string>>>&
  material_names() const {
    return material_names_;
  }

 private:
  const SamplesOptions& options_;
  cook::ModelCookOptions cook_options_;
  std::vector<std::pair<std::string, std::vector<std::string>>> material_names_;
};

// Returns `mesh` with every position multiplied by `scale` (positive, so
// normals and tangents are unchanged for an axis-aligned box or plane).
asset::MeshData Scaled(asset::MeshData mesh, glm::vec3 scale) {
  for (asset::Vertex& vertex : mesh.vertices) {
    vertex.position *= scale;
  }
  mesh.bounds = asset::ComputeBounds(mesh.vertices);
  return mesh;
}

bool CookRifle(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  cook::ModelCookOptions options = cooker.cook_options();
  options.merge_static_nodes = true;
  options.scale = weapon::M16Geometry().scale;
  auto cooked = cook::CookModel(cooker.Source("m16.obj"), options);
  if (!cooked.ok()) {
    core::log::Error("blue-pack samples: m16.obj: {}",
                     cook::ToString(cooked.error()));
    return false;
  }
  prefab::Prefab prefab = std::move(cooked).value();
  AddWeaponSockets(prefab, weapon::M16Geometry());
  return cooker.Save(prefab, "m16", start);
}

bool CookPistol(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  cook::ModelCookOptions options = cooker.cook_options();
  options.scale = weapon::Glock17Geometry().scale;
  options.drop_nodes = {kPistolDuplicateNode};
  auto cooked = cook::CookModel(cooker.Source("glock/glock17.glb"), options);
  if (!cooked.ok()) {
    core::log::Error("blue-pack samples: glock17.glb: {}",
                     cook::ToString(cooked.error()));
    return false;
  }
  prefab::Prefab prefab = std::move(cooked).value();
  std::vector<int> clips(prefab.clips.size());
  for (size_t i = 0; i < clips.size(); ++i) {
    clips[i] = static_cast<int>(i);
  }
  auto fire = cook::MergeClips(prefab, clips, std::string(kPistolFireClip));
  if (!fire.ok() ||
      !cook::AddClipEvent(prefab, fire.value(), "shot", kPistolShotTime).ok() ||
      !cook::AddClipEvent(prefab, fire.value(), "slide_lock",
                          kPistolSlideLockTime)
           .ok()) {
    core::log::Error("blue-pack samples: glock17: cannot build the fire clip");
    return false;
  }
  AddWeaponSockets(prefab, weapon::Glock17Geometry());
  return cooker.Save(prefab, "glock17", start);
}

bool CookHouse(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  cook::ModelCookOptions options = cooker.cook_options();
  options.merge_static_nodes = true;
  auto cooked = cook::CookModel(cooker.Source("house.obj"), options);
  if (!cooked.ok()) {
    core::log::Error("blue-pack samples: house.obj: {}",
                     cook::ToString(cooked.error()));
    return false;
  }
  prefab::Prefab prefab = std::move(cooked).value();
  return SampleCooker::Collider(prefab, prefab::ColliderKind::kTriangleMesh) &&
         cooker.Save(prefab, "house", start);
}

bool CookGround(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  std::optional<material::MaterialDesc> ground =
      cooker.Material("materials/ground.bmat");
  if (!ground) {
    return false;
  }
  // The .bmat's uv_scale compensates the old stretched 12 m plane; this
  // plane has the real size and UVs in metres.
  ground->uv_scale = kGroundUvScale;
  prefab::Prefab prefab;
  if (!cooker.AddMaterial(prefab, *ground)) {
    return false;
  }
  // MakePlane(1): u = x + 0.5, v = z + 0.5. Scaling positions and UVs by the
  // size keeps UVs in metres.
  const glm::vec3 size(kGroundWidth, 1.0f, kGroundBackZ - kGroundFrontZ);
  asset::MeshData plane = Scaled(asset::MakePlane(1.0f), size);
  for (asset::Vertex& vertex : plane.vertices) {
    vertex.uv0 *= glm::vec2(size.x, size.z);
    vertex.uv1 = vertex.uv0;
  }
  cook::AddMeshToPrefab(prefab, std::move(plane), -1, "ground");
  return SampleCooker::Collider(prefab, prefab::ColliderKind::kBox) &&
         cooker.Save(prefab, "ground", start);
}

bool CookTarget(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  std::optional<material::MaterialDesc> steel =
      cooker.Material("materials/steel.bmat");
  prefab::Prefab prefab;
  if (!steel || !cooker.AddMaterial(prefab, *steel)) {
    return false;
  }
  cook::AddMeshToPrefab(prefab, Scaled(asset::MakeCube(), kPlateSize), -1,
                        "plate");
  return SampleCooker::Collider(prefab, prefab::ColliderKind::kBox) &&
         cooker.Save(prefab, "target", start);
}

bool CookCrate(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  prefab::Prefab prefab;
  if (!cooker.AddMaterial(
          prefab, {.name = "crate", .base_color = {0.8f, 0.8f, 0.8f, 1.0f}})) {
    return false;
  }
  cook::AddMeshToPrefab(prefab, asset::MakeCube(), -1, "crate");
  return SampleCooker::Collider(prefab, prefab::ColliderKind::kBox,
                                /*dynamic=*/true) &&
         cooker.Save(prefab, "crate", start);
}

bool CookSphere(SampleCooker& cooker) {
  const Clock::time_point start = Clock::now();
  prefab::Prefab prefab;
  if (!cooker.AddMaterial(
          prefab, {.name = "sphere", .base_color = {0.8f, 0.8f, 0.8f, 1.0f}})) {
    return false;
  }
  cook::AddMeshToPrefab(prefab, asset::MakeSphere(kSphereSegments), -1,
                        "sphere");
  return SampleCooker::Collider(prefab, prefab::ColliderKind::kSphere) &&
         cooker.Save(prefab, "sphere", start);
}

std::string PreviewMaterialFile(std::string_view kind, std::string_view tag) {
  return std::format("materials/preview/{}_r{}.bmat", kind, tag);
}

// The material library: every preview material plus ground and steel. Fills
// `preview_names` with the material names, [row][column].
bool CookMaterialLibrary(
    SampleCooker& cooker,
    std::array<std::array<std::string, 5>, 2>& preview_names) {
  const Clock::time_point start = Clock::now();
  prefab::Prefab prefab;
  for (size_t row = 0; row < std::size(kPreviewKinds); ++row) {
    for (size_t i = 0; i < std::size(kPreviewRoughnessTags); ++i) {
      std::optional<material::MaterialDesc> desc = cooker.Material(
          PreviewMaterialFile(kPreviewKinds[row], kPreviewRoughnessTags[i]));
      if (!desc || !cooker.AddMaterial(prefab, *desc)) {
        return false;
      }
      preview_names[row][i] = desc->name;
    }
  }
  for (const char* file : {"materials/ground.bmat", "materials/steel.bmat"}) {
    std::optional<material::MaterialDesc> desc = cooker.Material(file);
    if (!desc || !cooker.AddMaterial(prefab, *desc)) {
      return false;
    }
  }
  return cooker.Save(prefab, "materials", start);
}

// Adds an instance of prefab file `file` (in the output directory).
scene::InstanceId Place(scene::SceneData& scene, const SampleCooker& cooker,
                        std::string_view file, std::string name,
                        const scene::Transform& transform) {
  scene::Instance instance{.name = std::move(name),
                           .prefab = scene::AddPrefab(scene, cooker.Out(file)),
                           .transform = transform};
  return scene::AddInstance(scene, std::move(instance));
}

bool CookRangeScene(
    SampleCooker& cooker,
    const std::array<std::array<std::string, 5>, 2>& preview_names) {
  const Clock::time_point start = Clock::now();

  // The HDRI travels with the scene.
  const std::string hdri = cooker.Out(kHdri);
  auto hdri_bytes =
      platform::ReadFile(cooker.Source(kHdri), std::size_t{256} * 1024 * 1024);
  if (!hdri_bytes.ok() ||
      !platform::WriteFileAtomic(hdri, hdri_bytes.value()).ok()) {
    core::log::Error("blue-pack samples: cannot copy {} to {}",
                     cooker.Source(kHdri), hdri);
    return false;
  }

  scene::SceneData scene;
  Place(scene, cooker, "ground.pbo", "ground",
        {.position = {0.0f, 0.0f, 0.5f * (kGroundBackZ + kGroundFrontZ)}});
  Place(scene, cooker, "house.pbo", "house", {.position = kHousePosition});
  Place(scene, cooker, "crate.pbo", "crate",
        {.position = kCratePosition,
         .rotation =
             glm::angleAxis(glm::radians(kCrateTiltDegrees),
                            glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)))});

  for (const RangeTarget& target : kRangeTargets) {
    glm::vec3 position =
        kFiringPosition + kDownRange * target.range + kRight * target.offset;
    position.y = kTargetCentreHeight;
    Place(scene, cooker, "target.pbo",
          std::format("{}{:.0f} m", kTargetNamePrefix, target.range),
          {.position = position,
           .scale = {1.0f, target.height / kPlateSize.y, 1.0f}});
  }

  const int library = scene::AddPrefab(scene, cooker.Out("materials.pbo"));
  for (size_t row = 0; row < std::size(kPreviewKinds); ++row) {
    for (size_t i = 0; i < std::size(kPreviewRoughnessTags); ++i) {
      scene::Instance sphere{
          .name = std::format("preview {} r{}", kPreviewKinds[row],
                              kPreviewRoughnessTags[i]),
          .prefab = scene::AddPrefab(scene, cooker.Out("sphere.pbo")),
          .transform = {
              .position =
                  kPreviewFirstSphere +
                  glm::vec3(kPreviewSpacing * static_cast<float>(i), 0.0f,
                            -kPreviewRowSpacing * static_cast<float>(row))}};
      sphere.materials.push_back({.material_name = "sphere",
                                  .source_prefab = library,
                                  .source_material = preview_names[row][i]});
      scene::AddInstance(scene, std::move(sphere));
    }
  }

  scene.sun.direction = scene::kDefaultSunDirection;
  scene.sun.color = scene::kDefaultSunColor;
  scene.sun.intensity = scene::kDefaultSunIntensity;
  scene.point_lights.push_back({.name = "lamp",
                                .position = kLampPosition,
                                .color = kLampColor,
                                .intensity = scene::kDefaultPointLightIntensity,
                                .range = kLampRange});
  scene.environment = {.hdri_path = hdri,
                       .intensity = scene::kDefaultEnvironmentIntensity,
                       .rotation_radians = scene::kDefaultEnvironmentRotation,
                       .exposure_ev = scene::kDefaultExposureEv};
  // Yaw 0 looks along -Z, down range.
  scene.player_start = {.position = kFiringPosition};

  // Every override must name materials that exist.
  std::vector<std::vector<std::string>> names(scene.prefabs.size());
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    for (const auto& [path, materials] : cooker.material_names()) {
      if (core::NormalizePath(path) == core::NormalizePath(scene.prefabs[i])) {
        names[i] = materials;
      }
    }
  }
  const std::vector<scene::SceneIssue> issues =
      scene::ValidateAgainstPrefabs(scene, names);
  for (const scene::SceneIssue& issue : issues) {
    core::log::Error("blue-pack samples: range.mscn: {}", issue.message);
  }
  const std::string path = cooker.Out("range.mscn");
  if (!issues.empty()) {
    return false;
  }
  if (auto saved = scene::SaveMscn(scene, path); !saved.ok()) {
    core::log::Error("blue-pack samples: cannot write {}: {}", path,
                     scene::ToString(saved.error()));
    return false;
  }
  core::log::Info(
      "sample range.mscn built in {:.0f} ms ({} instances)",
      std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
      scene.instances.size());
  return true;
}

}  // namespace

bool CookSamples(const SamplesOptions& options) {
  const Clock::time_point start = Clock::now();
  if (!platform::CreateDirectories(options.out_dir)) {
    core::log::Error("blue-pack samples: cannot create {}", options.out_dir);
    return false;
  }
  physics::PhysicsRuntime runtime;
  SampleCooker cooker(options);
  std::array<std::array<std::string, 5>, 2> preview_names;
  const bool ok =
      CookRifle(cooker) && CookPistol(cooker) && CookHouse(cooker) &&
      CookGround(cooker) && CookTarget(cooker) && CookCrate(cooker) &&
      CookSphere(cooker) && CookMaterialLibrary(cooker, preview_names) &&
      CookRangeScene(cooker, preview_names);
  core::log::Info("blue-pack samples: {} in {:.1f} s ({} textures)",
                  ok ? "done" : "FAILED",
                  std::chrono::duration<double>(Clock::now() - start).count(),
                  options.fast ? "fast" : "high quality");
  return ok;
}

}  // namespace blue::pack
