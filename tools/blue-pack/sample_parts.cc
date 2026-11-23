#include "tools/blue-pack/sample_parts.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "cook/prefab_split.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "weapon/weapon_geometry.h"

namespace blue::pack {
namespace {

using PartsResult = core::Result<std::vector<SamplePart>, std::string>;
using Status = core::Result<void, std::string>;

constexpr std::string_view kPartsDir = "weapons/glock17/parts/";

// The sample pistol's bore points along -X (weapon_geometry.h).
constexpr glm::vec3 kPistolForward{-1.0f, 0.0f, 0.0f};

// Iron sights on the sample Slide, model units, from the measurements in
// weapon::Glock17Geometry()'s comment: the slide's top flat is at y 0.759;
// the rear sight spans x [0.872, 0.913], ears |z| 0.033 .. 0.085 up to
// y 0.810, notch bottom y 0.784; the front post spans x [-1.130, -1.043],
// |z| < 0.023, top y 0.819.
constexpr float kSlideFlatY = 0.759f;
constexpr float kRearSightMinX = 0.872f;
constexpr float kRearSightMaxX = 0.913f;
constexpr float kRearEarInnerZ = 0.033f;
constexpr float kRearEarOuterZ = 0.085f;
constexpr float kRearEarTopY = 0.810f;
constexpr float kRearNotchBottomY = 0.784f;
constexpr float kFrontSightMinX = -1.130f;
constexpr float kFrontSightMaxX = -1.043f;
constexpr float kFrontSightHalfZ = 0.023f;
constexpr float kFrontSightTopY = 0.819f;
// The placeholder sights are this much larger on every side than the
// moulded ones they cover, so they show.
constexpr float kSightOversize = 0.0005f;  // m

// Grip bands (model units) where the Frame's vertices give the grip's
// front-to-back centre; the line through both centres is the grip axis.
// Behind x 0.3 the bands hold only the grip (Glock17Geometry(): the grip
// spans x [0.38, 1.0] at y -0.2 and [0.51, 1.19] at y -0.6).
constexpr float kUpperBandMinY = -0.25f;
constexpr float kUpperBandMaxY = -0.15f;
constexpr float kLowerBandMinY = -0.65f;
constexpr float kLowerBandMaxY = -0.55f;
constexpr float kGripMinX = 0.3f;
// The front of the frame (model units behind its front end) holds only the
// dust cover with the accessory rail; its lowest vertex is the rail face.
constexpr float kRailSectionLength = 0.4f;

// Measured on the user's Glock17_Gen5 prefab (metres) so parts of both
// models fit each other's slots: the magazine's top (feed lips) is 4.5 mm
// below the bore (bore y 0.1263, Magazine top y 0.1218), its floor 8.2 mm
// below the frame (Magazine min y 0, Frame min y 0.0082), and the light's
// top centre 27 mm behind the frame's front (Frame max z 0.0945, Olight_PL2
// z [0.0254, 0.1095]) and 4.7 mm above the rail's lowest face (Frame min y
// 0.0983 in its front 34 mm, Olight_PL2 max y 0.103).
constexpr float kFeedLipsBelowBore = 0.0045f;
constexpr float kFloorBelowFrame = 0.0082f;
constexpr float kTacticalBehindFrameFront = 0.027f;
constexpr float kTacticalAboveRailFace = 0.0047f;

// Placeholder sizes (m). Magazine: Gen5 magazine width 22.2 mm, a Glock 17
// magazine is about 31 mm front to back; the 21- and 33-round magazines are
// 22 mm and 98 mm longer (G17 magazine ~128 mm, G33 ~226 mm).
constexpr float kMagazineWidth = 0.022f;
constexpr float kMagazineDepth = 0.031f;
constexpr float kMagazine21Extra = 0.022f;
constexpr float kMagazine33Extra = 0.098f;
// Thread stub on the threaded barrel (1/2x28-like, 12 mm long).
constexpr float kThreadRadius = 0.0048f;
constexpr float kThreadLength = 0.012f;
// 9 mm suppressor (33 mm x 165 mm) and compensator (23 mm x 32 mm).
constexpr float kSuppressorRadius = 0.0165f;
constexpr float kSuppressorLength = 0.165f;
constexpr float kCompensatorRadius = 0.0115f;
constexpr float kCompensatorLength = 0.032f;
// Micro red dot (RMR-like): base plate, a hood at the front with an open
// window, the lens disc in the hood.
constexpr float kRedDotRearZ = -0.011f;  // stays on the slide (11.7 mm)
constexpr float kRedDotFrontZ = 0.034f;
constexpr float kRedDotHalfWidth = 0.013f;
constexpr float kRedDotBaseTop = 0.007f;
constexpr float kRedDotHoodRearZ = 0.022f;
constexpr float kRedDotWindowHalfWidth = 0.0105f;
constexpr float kRedDotRoofBottom = 0.025f;
constexpr float kRedDotRoofTop = 0.028f;
constexpr float kRedDotLensRadius = 0.0085f;
constexpr int kCylinderSegments = 24;

const material::MaterialDesc& PlaceholderMaterial() {
  static const material::MaterialDesc desc{
      .name = "placeholder",
      .base_color = {0.05f, 0.05f, 0.055f, 1.0f},
      .metallic = 0.6f,
      .roughness = 0.45f};
  return desc;
}

int FindMesh(const prefab::Prefab& prefab, std::string_view name) {
  for (size_t i = 0; i < prefab.meshes.size(); ++i) {
    if (prefab.meshes[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Returns a box from `min` to `max` (MakeCube scaled and moved).
asset::MeshData Box(glm::vec3 min, glm::vec3 max) {
  asset::MeshData mesh = asset::MakeCube();
  const glm::vec3 size = max - min;
  const glm::vec3 centre = (min + max) * 0.5f;
  for (asset::Vertex& vertex : mesh.vertices) {
    vertex.position = vertex.position * size + centre;
  }
  mesh.bounds = asset::ComputeBounds(mesh.vertices);
  return mesh;
}

// Returns a closed cylinder of `radius` along +Z from z0 to z1.
asset::MeshData Cylinder(float radius, float z0, float z1) {
  asset::MeshData mesh;
  const auto add = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv, glm::vec3 t) {
    mesh.vertices.push_back({.position = p,
                             .normal = n,
                             .uv0 = uv,
                             .uv1 = uv,
                             .tangent = glm::vec4(t, 1.0f)});
    return static_cast<asset::Index>(mesh.vertices.size() - 1);
  };
  const float step = glm::two_pi<float>() / kCylinderSegments;
  // Side: a seam-duplicated ring at each end, outward normals.
  for (int i = 0; i <= kCylinderSegments; ++i) {
    const float a = step * static_cast<float>(i);
    const glm::vec3 n(std::cos(a), std::sin(a), 0.0f);
    const glm::vec3 t(-n.y, n.x, 0.0f);
    const float u = static_cast<float>(i) / kCylinderSegments;
    add(glm::vec3(n.x * radius, n.y * radius, z0), n, {u, 1.0f}, t);
    add(glm::vec3(n.x * radius, n.y * radius, z1), n, {u, 0.0f}, t);
  }
  for (int i = 0; i < kCylinderSegments; ++i) {
    const auto base = static_cast<asset::Index>(2 * i);
    mesh.indices.insert(mesh.indices.end(), {base, base + 2, base + 1, base + 1,
                                             base + 2, base + 3});
  }
  // Caps: a centre vertex and a ring each.
  for (const float z : {z0, z1}) {
    const glm::vec3 n(0.0f, 0.0f, z == z1 ? 1.0f : -1.0f);
    const asset::Index centre =
        add({0.0f, 0.0f, z}, n, {0.5f, 0.5f}, {1.0f, 0.0f, 0.0f});
    for (int i = 0; i <= kCylinderSegments; ++i) {
      const float a = step * static_cast<float>(i);
      const glm::vec2 c(std::cos(a), std::sin(a));
      add(glm::vec3(c * radius, z), n, c * 0.5f + 0.5f, {1.0f, 0.0f, 0.0f});
    }
    for (int i = 0; i < kCylinderSegments; ++i) {
      const auto a = static_cast<asset::Index>(centre + 1 + i);
      if (z == z1) {
        mesh.indices.insert(mesh.indices.end(), {centre, a, a + 1});
      } else {
        mesh.indices.insert(mesh.indices.end(), {centre, a + 1, a});
      }
    }
  }
  mesh.submeshes.push_back(
      {.index_count = static_cast<uint32_t>(mesh.indices.size())});
  mesh.bounds = asset::ComputeBounds(mesh.vertices);
  return mesh;
}

// Rotates `mesh` by `rotation` about the origin, then moves it by `offset`.
asset::MeshData Moved(asset::MeshData mesh, glm::quat rotation,
                      glm::vec3 offset) {
  for (asset::Vertex& vertex : mesh.vertices) {
    vertex.position = rotation * vertex.position + offset;
    vertex.normal = rotation * vertex.normal;
    vertex.tangent =
        glm::vec4(rotation * glm::vec3(vertex.tangent), vertex.tangent.w);
  }
  mesh.bounds = asset::ComputeBounds(mesh.vertices);
  return mesh;
}

// Appends `from` to `into` as part of its single submesh.
void Append(asset::MeshData& into, const asset::MeshData& from) {
  const auto base = static_cast<asset::Index>(into.vertices.size());
  into.vertices.insert(into.vertices.end(), from.vertices.begin(),
                       from.vertices.end());
  for (const asset::Index index : from.indices) {
    into.indices.push_back(base + index);
  }
  into.submeshes = {
      {.index_count = static_cast<uint32_t>(into.indices.size())}};
  into.bounds = asset::ComputeBounds(into.vertices);
}

// Where the parts sit on the sample pistol, in its prefab space (metres,
// bore along -X), derived from the cooked meshes.
struct PistolLayout {
  float bore_y = 0.0f;
  glm::vec3 muzzle{0.0f};
  glm::vec3 slide_origin{0.0f};   // slide rear face on the bore axis
  glm::vec3 barrel_origin{0.0f};  // barrel rear face on the bore axis
  glm::vec3 magazine_top{0.0f};   // top centre of a magazine
  glm::vec3 magazine_down{0.0f, -1.0f, 0.0f};  // unit, along the grip
  float magazine_length = 0.0f;                // 17 rounds, top to floor
  glm::vec3 tactical{0.0f};    // top centre of a light on the rail
  glm::vec3 rear_sight{0.0f};  // base centre on the slide flat
  glm::vec3 front_sight{0.0f};
};

// Returns the mean x of the extreme x of the Frame vertices (prefab space)
// in [min_y, max_y] behind min_x, i.e. the middle of the grip there.
std::optional<float> GripCentreX(const std::vector<glm::vec3>& frame,
                                 float min_y, float max_y, float min_x) {
  std::optional<float> lo;
  std::optional<float> hi;
  for (const glm::vec3& p : frame) {
    if (p.y < min_y || p.y > max_y || p.x < min_x) {
      continue;
    }
    lo = lo ? std::min(*lo, p.x) : p.x;
    hi = hi ? std::max(*hi, p.x) : p.x;
  }
  if (!lo) {
    return std::nullopt;
  }
  return (*lo + *hi) * 0.5f;
}

core::Result<PistolLayout, std::string> MeasurePistol(
    const prefab::Prefab& pistol) {
  using LayoutResult = core::Result<PistolLayout, std::string>;
  const weapon::WeaponGeometry& geometry = weapon::Glock17Geometry();
  const float s = geometry.scale;
  const int frame_mesh = FindMesh(pistol, "Frame");
  const int slide_mesh = FindMesh(pistol, "Slide");
  const int barrel_mesh = FindMesh(pistol, "Barrel");
  if (frame_mesh < 0 || slide_mesh < 0 || barrel_mesh < 0) {
    return LayoutResult::Error("the pistol lacks a Frame, Slide or Barrel");
  }
  const asset::Bounds frame = cook::MeshBoundsInPrefabSpace(pistol, frame_mesh);
  const asset::Bounds slide = cook::MeshBoundsInPrefabSpace(pistol, slide_mesh);
  const asset::Bounds barrel =
      cook::MeshBoundsInPrefabSpace(pistol, barrel_mesh);

  // The Frame's vertices in prefab space.
  const prefab::Mesh& mesh = pistol.meshes[static_cast<size_t>(frame_mesh)];
  const glm::mat4 to_prefab =
      cook::RestWorldMatrices(pistol)[static_cast<size_t>(mesh.node)];
  std::vector<glm::vec3> frame_points;
  frame_points.reserve(mesh.vertices.size());
  for (const asset::Vertex& vertex : mesh.vertices) {
    frame_points.emplace_back(to_prefab * glm::vec4(vertex.position, 1.0f));
  }

  PistolLayout layout;
  layout.bore_y = geometry.muzzle.y * s;
  layout.muzzle = glm::vec3(geometry.muzzle.x * s, layout.bore_y, 0.0f);
  layout.slide_origin = glm::vec3(slide.max.x, layout.bore_y, 0.0f);
  layout.barrel_origin = glm::vec3(barrel.max.x, layout.bore_y, 0.0f);

  // Magazine: down the grip axis from the feed lips to below the frame.
  const std::optional<float> upper = GripCentreX(
      frame_points, kUpperBandMinY * s, kUpperBandMaxY * s, kGripMinX * s);
  const std::optional<float> lower = GripCentreX(
      frame_points, kLowerBandMinY * s, kLowerBandMaxY * s, kGripMinX * s);
  if (!upper || !lower) {
    return LayoutResult::Error("no Frame vertices in the grip bands");
  }
  const float upper_y = 0.5f * (kUpperBandMinY + kUpperBandMaxY) * s;
  const float lower_y = 0.5f * (kLowerBandMinY + kLowerBandMaxY) * s;
  const glm::vec3 a(*upper, upper_y, 0.0f);
  const glm::vec3 b(*lower, lower_y, 0.0f);
  layout.magazine_down = glm::normalize(b - a);
  const float top_y = layout.bore_y - kFeedLipsBelowBore;
  const float floor_y = frame.min.y - kFloorBelowFrame;
  const float along = (top_y - a.y) / layout.magazine_down.y;
  layout.magazine_top = a + layout.magazine_down * along;
  layout.magazine_length = (top_y - floor_y) / -layout.magazine_down.y;

  // Light: on the rail face, behind the frame's front end.
  float rail_y = frame.max.y;
  const float rail_end_x = frame.min.x + kRailSectionLength * s;
  for (const glm::vec3& p : frame_points) {
    if (p.x <= rail_end_x) {
      rail_y = std::min(rail_y, p.y);
    }
  }
  layout.tactical = glm::vec3(frame.min.x + kTacticalBehindFrameFront,
                              rail_y + kTacticalAboveRailFace, 0.0f);

  layout.rear_sight =
      glm::vec3(0.5f * (kRearSightMinX + kRearSightMaxX), kSlideFlatY, 0.0f) *
      s;
  layout.front_sight =
      glm::vec3(0.5f * (kFrontSightMinX + kFrontSightMaxX), kSlideFlatY, 0.0f) *
      s;
  return LayoutResult::Ok(layout);
}

// The part frame of a part whose origin is `origin` (pistol prefab space).
struct PartFrame {
  glm::vec3 origin{0.0f};

  [[nodiscard]] glm::quat turn() const {
    return glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
  }
  // A pistol prefab-space point in this part's space.
  [[nodiscard]] glm::vec3 Point(glm::vec3 p) const {
    return turn() * (p - origin);
  }
  // A pistol prefab-space direction in this part's space.
  [[nodiscard]] glm::vec3 Direction(glm::vec3 d) const { return turn() * d; }
};

// The rotation that turns +Z onto unit `axis` with the least turn.
glm::quat RotationFromZ(glm::vec3 axis) {
  const glm::vec3 z(0.0f, 0.0f, 1.0f);
  const float cosine = glm::dot(z, axis);
  if (cosine > 0.999999f) {
    return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  }
  return glm::angleAxis(std::acos(cosine), glm::normalize(glm::cross(z, axis)));
}

// Adds socket `name` at `frame` in part space on node `node_name` of `part`
// ("" = the part root).
Status AddSocket(prefab::Prefab& part, std::string name,
                 std::string_view node_name,
                 const animation::Transform& frame) {
  int node = -1;
  if (!node_name.empty()) {
    node = animation::FindNode(part.hierarchy, node_name);
    if (node < 0) {
      return Status::Error(
          std::format("socket '{}': no node '{}'", name, node_name));
    }
  }
  auto socket = cook::SocketAtPrefabFrame(part, std::move(name), node, frame);
  if (!socket.ok()) {
    return Status::Error(socket.error());
  }
  part.sockets.push_back(std::move(socket).value());
  return Status::Ok();
}

Status AddPoint(prefab::Prefab& part, std::string name,
                std::string_view node_name, glm::vec3 point) {
  return AddSocket(part, std::move(name), node_name, {.translation = point});
}

// Adds `mesh` with the placeholder material to `part` at its root.
Status AddPlaceholderMesh(prefab::Prefab& part, asset::MeshData mesh,
                          std::string name,
                          const cook::ModelCookOptions& cook_options) {
  int material = prefab::FindMaterial(part, PlaceholderMaterial().name);
  if (material < 0) {
    auto added =
        cook::AddMaterialToPrefab(part, PlaceholderMaterial(), cook_options);
    if (!added.ok()) {
      return Status::Error(std::format("placeholder material: {}",
                                       cook::ToString(added.error())));
    }
    material = added.value();
  }
  for (asset::Submesh& submesh : mesh.submeshes) {
    submesh.material = material;
  }
  cook::AddMeshToPrefab(part, std::move(mesh), -1, std::move(name));
  part.bounds = cook::ComputePrefabBounds(part);
  return Status::Ok();
}

// Collects the parts; the first failure sticks.
class PartList {
 public:
  explicit PartList(const cook::ModelCookOptions& cook_options)
      : cook_options_(cook_options) {}

  const cook::ModelCookOptions& cook_options() const { return cook_options_; }

  // Cuts `nodes` out of `pistol` with its origin at `origin`.
  std::optional<prefab::Prefab> Split(const prefab::Prefab& pistol,
                                      std::vector<std::string> nodes,
                                      std::vector<std::string> exclude,
                                      std::optional<glm::vec3> origin,
                                      bool keep_skeleton) {
    cook::SplitOptions options{.nodes = std::move(nodes),
                               .exclude = std::move(exclude),
                               .origin_point = origin,
                               .forward = kPistolForward,
                               .keep_skeleton = keep_skeleton};
    auto part = cook::SplitPrefab(pistol, options);
    if (!part.ok()) {
      Fail(part.error());
      return std::nullopt;
    }
    return std::move(part).value();
  }

  void Check(const Status& status) {
    if (!status.ok()) {
      Fail(status.error());
    }
  }
  void Fail(std::string message) {
    if (error_.empty()) {
      error_ = std::move(message);
    }
  }

  void Add(std::string_view file, std::string_view id, prefab::Prefab part) {
    prefab::SetTag(part, kPartTag, id);
    part.bounds = cook::ComputePrefabBounds(part);
    parts_.push_back({.path = std::format("{}{}", kPartsDir, file),
                      .prefab = std::move(part)});
  }

  // A part made of placeholder meshes only.
  void AddPlaceholder(
      std::string_view file, std::string_view id, asset::MeshData mesh,
      const std::vector<std::pair<std::string, animation::Transform>>&
          sockets) {
    prefab::Prefab part;
    Check(AddPlaceholderMesh(part, std::move(mesh), std::string(id),
                             cook_options_));
    for (const auto& [name, frame] : sockets) {
      Check(AddSocket(part, name, "", frame));
    }
    Add(file, id, std::move(part));
  }

  PartsResult Finish() {
    if (!error_.empty()) {
      return PartsResult::Error(error_);
    }
    return PartsResult::Ok(std::move(parts_));
  }

 private:
  const cook::ModelCookOptions& cook_options_;
  std::vector<SamplePart> parts_;
  std::string error_;
};

animation::Transform At(glm::vec3 point) { return {.translation = point}; }

void AddFrame(PartList& list, const prefab::Prefab& pistol,
              const PistolLayout& layout) {
  // The frame keeps the pistol's origin; only the turn applies.
  const PartFrame part_frame{};
  // Barrel and Slide are children of Frame in the model: left out.
  std::optional<prefab::Prefab> frame =
      list.Split(pistol, {"Frame", "Trigger", "Frame_Parts", "Guide_Rod"},
                 {"Barrel", "Slide"}, glm::vec3(0.0f), /*keep_skeleton=*/true);
  if (!frame) {
    return;
  }
  // The whole pistol's muzzle and sight now come from the barrel and slide.
  frame->sockets.clear();
  const weapon::WeaponGeometry& geometry = weapon::Glock17Geometry();
  list.Check(AddPoint(*frame, "grip", "",
                      part_frame.Point(geometry.grip * geometry.scale)));
  list.Check(AddPoint(*frame, "slot.slide", "Slide",
                      part_frame.Point(layout.slide_origin)));
  list.Check(AddPoint(*frame, "slot.barrel", "Barrel",
                      part_frame.Point(layout.barrel_origin)));
  list.Check(AddPoint(*frame, "slot.magazine", "Frame",
                      part_frame.Point(layout.magazine_top)));
  list.Check(AddPoint(*frame, "slot.tactical", "Frame",
                      part_frame.Point(layout.tactical)));
  list.Add("frame.pbo", "glock17_frame", std::move(*frame));
}

void AddSlides(PartList& list, const prefab::Prefab& pistol,
               const PistolLayout& layout) {
  const weapon::WeaponGeometry& geometry = weapon::Glock17Geometry();
  const PartFrame part_frame{.origin = layout.slide_origin};
  for (const auto& [file, id] :
       {std::pair{"slide.pbo", "glock17_slide"},
        std::pair{"slide_mos.pbo", "glock17_slide_mos"}}) {
    std::optional<prefab::Prefab> slide =
        list.Split(pistol, {"Slide"}, {}, layout.slide_origin, false);
    if (!slide) {
      return;
    }
    list.Check(AddSocket(*slide, "sight", "",
                         {.translation = part_frame.Point(
                              geometry.sight_eye_point * geometry.scale),
                          .rotation = RotationFromZ(part_frame.Direction(
                              glm::normalize(geometry.sight_axis)))}));
    list.Check(AddPoint(*slide, "slot.sight_rear", "",
                        part_frame.Point(layout.rear_sight)));
    list.Check(AddPoint(*slide, "slot.sight_front", "",
                        part_frame.Point(layout.front_sight)));
    list.Add(file, id, std::move(*slide));
  }
}

void AddBarrels(PartList& list, const prefab::Prefab& pistol,
                const PistolLayout& layout) {
  const PartFrame part_frame{.origin = layout.barrel_origin};
  const glm::vec3 muzzle = part_frame.Point(layout.muzzle);
  std::optional<prefab::Prefab> barrel =
      list.Split(pistol, {"Barrel"}, {}, layout.barrel_origin, false);
  if (!barrel) {
    return;
  }
  prefab::Prefab threaded = *barrel;
  list.Check(AddPoint(*barrel, "muzzle", "", muzzle));
  list.Add("barrel.pbo", "glock17_barrel", std::move(*barrel));

  list.Check(AddPlaceholderMesh(
      threaded, Cylinder(kThreadRadius, muzzle.z, muzzle.z + kThreadLength),
      "thread", list.cook_options()));
  list.Check(AddPoint(threaded, "muzzle", "",
                      muzzle + glm::vec3(0.0f, 0.0f, kThreadLength)));
  list.Check(AddPoint(threaded, "slot.muzzle", "", muzzle));
  list.Add("barrel_threaded.pbo", "glock17_barrel_threaded",
           std::move(threaded));
}

void AddMuzzleDevices(PartList& list) {
  struct Device {
    const char* file;
    const char* id;
    float radius;
    float length;
  };
  for (const Device& device :
       {Device{"suppressor.pbo", "suppressor_9mm", kSuppressorRadius,
               kSuppressorLength},
        Device{"compensator.pbo", "compensator_9mm", kCompensatorRadius,
               kCompensatorLength}}) {
    list.AddPlaceholder(device.file, device.id,
                        Cylinder(device.radius, 0.0f, device.length),
                        {{"mount", At(glm::vec3(0.0f))},
                         {"muzzle", At({0.0f, 0.0f, device.length})}});
  }
}

void AddMagazines(PartList& list, const PistolLayout& layout) {
  // In part space the grip runs down and back (-Z); the box is built along
  // -Y from the origin and tilted about X onto the grip axis.
  const PartFrame part_frame{.origin = layout.magazine_top};
  const glm::vec3 down = part_frame.Direction(layout.magazine_down);
  const glm::quat tilt =
      glm::angleAxis(std::atan2(-down.z, -down.y), glm::vec3(1.0f, 0.0f, 0.0f));
  struct Magazine {
    const char* file;
    const char* id;
    float extra;
  };
  for (const Magazine& magazine :
       {Magazine{"mag_17.pbo", "glock_mag_17", 0.0f},
        Magazine{"mag_21.pbo", "glock_mag_21", kMagazine21Extra},
        Magazine{"mag_33.pbo", "glock_mag_33", kMagazine33Extra}}) {
    const float length = layout.magazine_length + magazine.extra;
    asset::MeshData box =
        Box({-0.5f * kMagazineWidth, -length, -0.5f * kMagazineDepth},
            {0.5f * kMagazineWidth, 0.0f, 0.5f * kMagazineDepth});
    list.AddPlaceholder(magazine.file, magazine.id,
                        Moved(std::move(box), tilt, glm::vec3(0.0f)), {});
  }
}

void AddIronSights(PartList& list, const PistolLayout& layout) {
  const weapon::WeaponGeometry& geometry = weapon::Glock17Geometry();
  const float s = geometry.scale;
  const float o = kSightOversize;
  // Rear: a base up to the notch bottom and two ears, origin at the base
  // centre on the slide flat.
  const PartFrame rear{.origin = layout.rear_sight};
  const float half_length = 0.5f * (kRearSightMaxX - kRearSightMinX) * s + o;
  const float notch = (kRearNotchBottomY - kSlideFlatY) * s;
  const float top = (kRearEarTopY - kSlideFlatY) * s + o;
  const float outer = kRearEarOuterZ * s + o;
  const float inner = kRearEarInnerZ * s;
  asset::MeshData rear_mesh =
      Box({-outer, 0.0f, -half_length}, {outer, notch, half_length});
  Append(rear_mesh,
         Box({-outer, notch, -half_length}, {-inner, top, half_length}));
  Append(rear_mesh,
         Box({inner, notch, -half_length}, {outer, top, half_length}));
  list.AddPlaceholder(
      "sight_rear.pbo", "glock_sight_rear", std::move(rear_mesh),
      {{"sight",
        {.translation = rear.Point(geometry.sight_eye_point * s) +
                        glm::vec3(0.0f, o, 0.0f),
         .rotation = RotationFromZ(
             rear.Direction(glm::normalize(geometry.sight_axis)))}}});

  // Front: a post, origin at its base centre.
  const float front_half_length =
      0.5f * (kFrontSightMaxX - kFrontSightMinX) * s + o;
  const float front_half_width = kFrontSightHalfZ * s + o;
  const float front_top = (kFrontSightTopY - kSlideFlatY) * s + o;
  list.AddPlaceholder("sight_front.pbo", "glock_sight_front",
                      Box({-front_half_width, 0.0f, -front_half_length},
                          {front_half_width, front_top, front_half_length}),
                      {});
}

void AddRedDot(PartList& list) {
  const float w = kRedDotHalfWidth;
  asset::MeshData mesh =
      Box({-w, 0.0f, kRedDotRearZ}, {w, kRedDotBaseTop, kRedDotFrontZ});
  Append(mesh,
         Box({-w, kRedDotBaseTop, kRedDotHoodRearZ},
             {-kRedDotWindowHalfWidth, kRedDotRoofBottom, kRedDotFrontZ}));
  Append(mesh, Box({kRedDotWindowHalfWidth, kRedDotBaseTop, kRedDotHoodRearZ},
                   {w, kRedDotRoofBottom, kRedDotFrontZ}));
  Append(mesh, Box({-w, kRedDotRoofBottom, kRedDotHoodRearZ},
                   {w, kRedDotRoofTop, kRedDotFrontZ}));
  // The lens disc is centred in the window, at the hood's front.
  const float axis_y = 0.5f * (kRedDotBaseTop + kRedDotRoofBottom);
  list.AddPlaceholder("reddot.pbo", "reddot_micro", std::move(mesh),
                      {{"sight", At({0.0f, axis_y, kRedDotRearZ})},
                       {"lens",
                        {.translation = {0.0f, axis_y, kRedDotFrontZ},
                         .scale = glm::vec3(kRedDotLensRadius)}}});
}

}  // namespace

core::Result<std::vector<SamplePart>, std::string> MakeGlockParts(
    const prefab::Prefab& pistol, const cook::ModelCookOptions& cook_options) {
  core::Result<PistolLayout, std::string> layout = MeasurePistol(pistol);
  if (!layout.ok()) {
    return PartsResult::Error(layout.error());
  }
  PartList list(cook_options);
  AddFrame(list, pistol, layout.value());
  AddSlides(list, pistol, layout.value());
  AddBarrels(list, pistol, layout.value());
  AddMuzzleDevices(list);
  AddMagazines(list, layout.value());
  AddIronSights(list, layout.value());
  AddRedDot(list);
  return list.Finish();
}

std::string BaseCatalogText() {
  // Glock 17 numbers after Escape from Tarkov (ergonomics 90, recoil
  // 311 / 260, 114 mm barrel -2 ergonomics -2.9 % recoil, slide +3);
  // weights realistic, the stock preset 0.905 kg loaded (magazine weights
  // include the rounds, 12.3 g each). Rifles: today's weapon_specs.cc
  // values; recoil converted with the Glock's scale (311 = look_kick_pitch
  // 0.008 rad, 260 = look_kick_yaw 0.004 rad).
  return R"(# Weapon parts catalogue: the sample Glock 17 parts and the whole rifles.
# Written by `blue-pack samples`. Edit freely: an edited file is kept and the
# fresh one goes to base.generated.cfg, which the loader skips (delete this
# file to regenerate it).
# Format and rules: out/briefs/G0-gunsmith-contract.md.
# Models marked "placeholder" are generated boxes and cylinders, temporary.

# --- Glock 17 -------------------------------------------------------------

[part glock17_frame]
name = Glock 17 frame
category = lower_receiver
prefab = weapons/glock17/parts/frame.pbo
weight = 0.21
ergonomics = 90
weapon_kind = pistol
recoil_vertical = 311
recoil_horizontal = 260
fire_modes = semi
fire_rate = 1100
slot.slide = required glock17_slide, glock17_slide_mos
slot.barrel = required glock17_barrel, glock17_barrel_threaded
slot.magazine = optional glock_mag_17, glock_mag_21, glock_mag_33
slot.tactical = optional

[part glock17_slide]
name = Glock 17 slide
category = slide
prefab = weapons/glock17/parts/slide.pbo
weight = 0.275
ergonomics = 3
sighting_range = 25
slot.sight_rear = optional glock_sight_rear
slot.sight_front = optional glock_sight_front

[part glock17_slide_mos]
name = Glock 17 MOS slide (optic cut)
category = slide
prefab = weapons/glock17/parts/slide_mos.pbo
weight = 0.28
ergonomics = 2
sighting_range = 25
slot.sight_rear = optional glock_sight_rear, reddot_micro
slot.sight_front = optional glock_sight_front

[part glock17_barrel]
name = Glock 17 9x19 114mm barrel
category = barrel
prefab = weapons/glock17/parts/barrel.pbo
weight = 0.12
ergonomics = -2
recoil = -2.9
accuracy_moa = 4.4
muzzle_velocity = 375

[part glock17_barrel_threaded]
name = Glock 17 9x19 threaded barrel (placeholder thread)
category = barrel
prefab = weapons/glock17/parts/barrel_threaded.pbo
weight = 0.13
ergonomics = -3
recoil = -3.5
accuracy_moa = 4.1
muzzle_velocity = 380
slot.muzzle = optional suppressor_9mm, compensator_9mm

[part glock_mag_17]
name = Glock 9x19 17-round magazine (placeholder)
category = magazine
prefab = weapons/glock17/parts/mag_17.pbo
weight = 0.29
ergonomics = 0
capacity = 17

[part glock_mag_21]
name = Glock 9x19 21-round magazine (placeholder)
category = magazine
prefab = weapons/glock17/parts/mag_21.pbo
weight = 0.35
ergonomics = -3
capacity = 21

[part glock_mag_33]
name = Glock 9x19 33-round magazine (placeholder)
category = magazine
prefab = weapons/glock17/parts/mag_33.pbo
weight = 0.53
ergonomics = -10
capacity = 33

[part glock_sight_rear]
name = Glock rear sight (placeholder)
category = sight_rear
prefab = weapons/glock17/parts/sight_rear.pbo
weight = 0.006
ergonomics = 0
sighting_range = 50

[part glock_sight_front]
name = Glock front sight (placeholder)
category = sight_front
prefab = weapons/glock17/parts/sight_front.pbo
weight = 0.004
ergonomics = 0

[part reddot_micro]
name = Micro red dot sight (placeholder)
category = optic
prefab = weapons/glock17/parts/reddot.pbo
weight = 0.045
ergonomics = -1
sighting_range = 100

[part suppressor_9mm]
name = 9x19 suppressor (placeholder)
category = muzzle
prefab = weapons/glock17/parts/suppressor.pbo
weight = 0.2
ergonomics = -10
recoil = -7
accuracy = -3
velocity = 0.99

[part compensator_9mm]
name = 9x19 compensator (placeholder)
category = muzzle
prefab = weapons/glock17/parts/compensator.pbo
weight = 0.035
ergonomics = -2
recoil = -5

[preset glock17]
name = Glock 17
root = glock17_frame
slide = glock17_slide
slide.sight_rear = glock_sight_rear
slide.sight_front = glock_sight_front
barrel = glock17_barrel
magazine = glock_mag_17

# --- Whole rifles (single-piece weapons, today's numbers) --------------------

[part m16]
name = M16A2
category = complete
prefab = m16.pbo
weight = 3.99
ergonomics = 50
weapon_kind = rifle
recoil_vertical = 175
recoil_horizontal = 163
fire_modes = semi, auto
fire_rate = 800
accuracy_moa = 3
muzzle_velocity = 948
capacity = 30
sighting_range = 300
legacy_type = m16

[part spr_a3g]
name = FN SPR A3G
category = complete
weight = 6.5
ergonomics = 35
weapon_kind = rifle
recoil_vertical = 467
recoil_horizontal = 195
fire_modes = semi
fire_rate = 50
accuracy_moa = 0.8
muzzle_velocity = 790
capacity = 5
sighting_range = 300
legacy_type = spr_a3g

[preset m16]
name = M16A2
root = m16

[preset spr_a3g]
name = FN SPR A3G
root = spr_a3g
)";
}

}  // namespace blue::pack
