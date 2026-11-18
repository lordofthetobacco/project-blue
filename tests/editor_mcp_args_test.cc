#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "editor/mcp/mcp_args.h"
#include "editor/mcp/mcp_export.h"
#include "editor/mcp/png_encode.h"
#include "mcp/json.h"
#include "prefab/prefab.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

using mcp::Json;

// Parses `text`, which must be valid JSON.
Json Parse(const char* text) {
  auto parsed = mcp::ParseJson(text);
  REQUIRE(parsed.ok());
  return parsed.value();
}

TEST_CASE("Vector arguments are read, absent or rejected") {
  const Json args = Parse(R"({"p": [1, 2.5, -3], "bad": [1, "x", 3],
                              "short": [1, 2], "inf": null})");
  auto p = GetVec3Arg(args, "p");
  REQUIRE(p.ok());
  REQUIRE(p.value().has_value());
  CHECK(*p.value() == glm::vec3(1.0f, 2.5f, -3.0f));
  CHECK_FALSE(GetVec3Arg(args, "missing").value().has_value());
  CHECK_FALSE(GetVec3Arg(args, "inf").value().has_value());  // null = absent
  CHECK_FALSE(GetVec3Arg(args, "bad").ok());
  CHECK_FALSE(GetVec3Arg(args, "short").ok());
  CHECK(GetVec2Arg(args, "short").ok());
}

TEST_CASE("Number arguments are range checked") {
  const Json args = Parse(R"({"r": 0.25, "big": 2, "s": "x"})");
  CHECK(*GetFloatArg(args, "r", 0.0f, 1.0f).value() == doctest::Approx(0.25));
  CHECK_FALSE(GetFloatArg(args, "big", 0.0f, 1.0f).ok());
  CHECK_FALSE(GetFloatArg(args, "s", 0.0f, 1.0f).ok());
  CHECK_FALSE(GetFloatArg(args, "none", 0.0f, 1.0f).value().has_value());
}

TEST_CASE("Instance ids and string lists accept one or many") {
  auto one = GetInstanceIdsArg(Parse(R"({"ids": 7})"), "ids");
  REQUIRE(one.ok());
  CHECK(one.value() == std::vector<scene::InstanceId>{7});
  auto many = GetInstanceIdsArg(Parse(R"({"ids": [3, 4]})"), "ids");
  CHECK(many.value() == std::vector<scene::InstanceId>{3, 4});
  CHECK_FALSE(GetInstanceIdsArg(Parse(R"({"ids": [0]})"), "ids").ok());
  CHECK_FALSE(GetInstanceIdsArg(Parse(R"({"ids": "x"})"), "ids").ok());

  auto lines = GetStringListArg(Parse(R"({"c": ["a", "b"]})"), "c");
  CHECK(lines.value() == std::vector<std::string>{"a", "b"});
  CHECK(GetStringListArg(Parse(R"({"c": "x"})"), "c").value().size() == 1);
  CHECK_FALSE(GetStringListArg(Parse(R"({"c": [1]})"), "c").ok());
}

TEST_CASE("Command words are quoted when needed and refused when impossible") {
  CHECK(QuoteCommandWord("crate").value() == "crate");
  CHECK(QuoteCommandWord("my crate").value() == "\"my crate\"");
  CHECK(QuoteCommandWord("a;b").value() == "\"a;b\"");
  CHECK(QuoteCommandWord("").value() == "\"\"");
  CHECK_FALSE(QuoteCommandWord("say \"hi\"").ok());
  CHECK_FALSE(QuoteCommandWord("two\nlines").ok());
  const std::vector<std::string> words{"open", "C:/My Assets/a.pbo"};
  CHECK(BuildCommand(words).value() == "open \"C:/My Assets/a.pbo\"");
}

TEST_CASE("Transform arguments set only the given parts") {
  scene::Transform transform;
  transform.scale = glm::vec3(2.0f);
  REQUIRE(
      ApplyTransformArgs(Parse(R"({"position": [1, 2, 3]})"), transform).ok());
  CHECK(transform.position == glm::vec3(1.0f, 2.0f, 3.0f));
  CHECK(transform.scale == glm::vec3(2.0f));

  REQUIRE(ApplyTransformArgs(Parse(R"({"rotation_deg": [0, 90, 0],
                                       "scale": 3})"),
                             transform)
              .ok());
  const glm::vec3 forward = transform.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
  CHECK(forward.x == doctest::Approx(-1.0f).epsilon(1e-4));
  CHECK(transform.scale == glm::vec3(3.0f));

  REQUIRE(ApplyTransformArgs(Parse(R"({"rotation": [0, 0, 0, 2]})"), transform)
              .ok());
  CHECK(transform.rotation.w == doctest::Approx(1.0f));  // Normalized.

  const scene::Transform before = transform;
  CHECK_FALSE(ApplyTransformArgs(Parse("{}"), transform).ok());
  CHECK_FALSE(ApplyTransformArgs(Parse(R"({"position": [1, 2, 3],
                                           "scale": [1, 0, 1]})"),
                                 transform)
                  .ok());
  CHECK_FALSE(ApplyTransformArgs(Parse(R"({"rotation_deg": [0, 0, 0],
                                           "rotation": [0, 0, 0, 1]})"),
                                 transform)
                  .ok());
  // A failed call changes nothing.
  CHECK(transform.position == before.position);
  CHECK(transform.scale == before.scale);
}

TEST_CASE("Material arguments validate ranges before changing anything") {
  prefab::Material material;
  auto count =
      ApplyMaterialArgs(Parse(R"({"roughness": 0.2, "base_color": [1, 0, 0, 1],
                "alpha_mode": "mask", "double_sided": true})"),
                        material);
  REQUIRE(count.ok());
  CHECK(count.value() == 4);
  CHECK(material.roughness == doctest::Approx(0.2f));
  CHECK(material.base_color.r == 1.0f);
  CHECK(material.alpha_mode == material::AlphaMode::kMask);
  CHECK(material.double_sided);

  const prefab::Material before = material;
  CHECK_FALSE(ApplyMaterialArgs(Parse(R"({"metallic": 0.5,
                                          "roughness": 1.5})"),
                                material)
                  .ok());
  CHECK(material.metallic == before.metallic);
  CHECK_FALSE(ApplyMaterialArgs(Parse("{}"), material).ok());
  CHECK_FALSE(
      ApplyMaterialArgs(Parse(R"({"alpha_mode": "blend"})"), material).ok());
}

TEST_CASE("Override arguments set optional tweaks") {
  scene::MaterialOverride material;
  material.material_name = "paint";
  auto count = ApplyOverrideArgs(
      Parse(R"({"metallic": 1, "uv_scale": [2, 2]})"), material);
  REQUIRE(count.ok());
  CHECK(count.value() == 2);
  CHECK(material.metallic == 1.0f);
  CHECK(material.uv_scale == glm::vec2(2.0f));
  CHECK_FALSE(material.roughness.has_value());
  CHECK_FALSE(
      ApplyOverrideArgs(Parse(R"({"uv_scale": [0, 1]})"), material).ok());
}

TEST_CASE("Sun arguments use angles or a direction") {
  scene::SunLight sun;
  REQUIRE(ApplySunArgs(Parse(R"({"elevation_deg": 30, "azimuth_deg": 90,
                                 "intensity": 5})"),
                       sun)
              .ok());
  const scene::SunAngles angles = scene::SunAnglesFromDirection(sun.direction);
  CHECK(glm::degrees(angles.elevation) == doctest::Approx(30.0f).epsilon(1e-3));
  CHECK(glm::degrees(angles.azimuth) == doctest::Approx(90.0f).epsilon(1e-3));
  CHECK(sun.intensity == 5.0f);

  REQUIRE(ApplySunArgs(Parse(R"({"direction": [0, -2, 0]})"), sun).ok());
  CHECK(sun.direction.y == doctest::Approx(-1.0f));
  CHECK_FALSE(ApplySunArgs(Parse(R"({"direction": [0, -1, 0],
                                     "elevation_deg": 10})"),
                           sun)
                  .ok());
  CHECK_FALSE(ApplySunArgs(Parse(R"({"elevation_deg": 120})"), sun).ok());
  CHECK_FALSE(ApplySunArgs(Parse("{}"), sun).ok());
}

TEST_CASE("Environment, point light and body arguments") {
  scene::Environment environment;
  REQUIRE(ApplyEnvironmentArgs(Parse(R"({"rotation_deg": 180,
                                         "exposure_ev": -1})"),
                               environment)
              .ok());
  CHECK(environment.rotation_radians == doctest::Approx(3.14159f));
  CHECK(environment.exposure_ev == -1.0f);
  CHECK_FALSE(
      ApplyEnvironmentArgs(Parse(R"({"intensity": -1})"), environment).ok());

  scene::ScenePointLight light;
  REQUIRE(ApplyPointLightArgs(Parse(R"({"position": [0, 3, 0], "range": 4,
                                        "name": "lamp"})"),
                              light)
              .ok());
  CHECK(light.position.y == 3.0f);
  CHECK(light.range == 4.0f);
  CHECK(light.name == "lamp");
  CHECK_FALSE(ApplyPointLightArgs(Parse(R"({"range": 0})"), light).ok());

  prefab::Physics physics;
  auto body = ApplyBodyArgs(Parse(R"({"dynamic": true, "mass": 3})"), physics);
  REQUIRE(body.ok());
  CHECK(body.value() == 2);
  CHECK(physics.dynamic);
  CHECK(physics.mass == 3.0f);
  CHECK_FALSE(ApplyBodyArgs(Parse(R"({"mass": 0})"), physics).ok());
  CHECK(ParseColliderKindArg("hull") == prefab::ColliderKind::kConvexHull);
  CHECK_FALSE(ParseColliderKindArg("cone").has_value());
}

TEST_CASE("Clip edits become editor commands with resolved clip indices") {
  prefab::Prefab prefab;
  prefab.clips.resize(3);
  prefab.clips[0].name = "idle";
  prefab.clips[1].name = "walk cycle";
  prefab.clips[2].name = "run";
  CHECK(ClipEditCommand(Parse(R"({"action": "loop", "clip": "run",
                                  "loop": true})"),
                        prefab)
            .value() == "clip-loop 2 1");
  CHECK(ClipEditCommand(Parse(R"({"action": "trim", "clip": 1, "start": 0.5,
                                  "end": 1.25})"),
                        prefab)
            .value() == "clip-trim 1 0.5 1.25");
  CHECK(ClipEditCommand(Parse(R"({"action": "merge", "name": "all moves",
                                  "clips": ["idle", 2]})"),
                        prefab)
            .value() == "clip-merge \"all moves\" 0 2");
  CHECK(ClipEditCommand(Parse(R"({"action": "add_event", "clip": "idle",
                                  "event": "step", "time": 0.3})"),
                        prefab)
            .value() == "event-add 0 step 0.3");
  CHECK(ClipEditCommand(Parse(R"({"action": "remove_event", "clip": 0,
                                  "event_index": 1})"),
                        prefab)
            .value() == "event-remove 0 1");
  CHECK_FALSE(
      ClipEditCommand(Parse(R"({"action": "loop", "clip": "fly", "loop": 1})"),
                      prefab)
          .ok());
  CHECK_FALSE(ClipEditCommand(Parse(R"({"action": "trim", "clip": 0,
                                        "start": 2, "end": 1})"),
                              prefab)
                  .ok());
  CHECK_FALSE(
      ClipEditCommand(Parse(R"({"action": "explode", "clip": 0})"), prefab)
          .ok());
}

TEST_CASE("LimitText keeps short text and cuts long text at a line") {
  CHECK(LimitText("short", 100) == "short");
  const std::string cut = LimitText("line one\nline two\nline three", 14);
  CHECK(cut.starts_with("line one\n..."));
  CHECK(cut.find("more bytes") != std::string::npos);
}

TEST_CASE("Scene JSON lists instances, overrides, sun and environment") {
  scene::SceneData scene;
  const int crate = scene::AddPrefab(scene, "C:/assets/props/crate.pbo");
  scene::Instance group;
  group.name = "group";
  const scene::InstanceId parent = scene::AddInstance(scene, group);
  scene::Instance instance;
  instance.name = "crate";
  instance.prefab = crate;
  instance.parent = parent;
  instance.transform.position = glm::vec3(1.0f, 2.0f, 3.0f);
  scene::MaterialOverride paint;
  paint.material_name = "paint";
  paint.roughness = 0.75f;
  instance.materials.push_back(paint);
  const scene::InstanceId id = scene::AddInstance(scene, instance);

  const Json json = SceneToJson(scene, "C:/assets");
  const Json& instances = *json.GetArray("instances");
  REQUIRE(instances.size() == 2);
  CHECK(instances.array_items()[0].Find("prefab")->is_null());
  const Json& item = instances.array_items()[1];
  CHECK(item.GetInteger("id") == static_cast<int64_t>(id));
  CHECK(*item.GetString("prefab") == "props/crate.pbo");
  CHECK(item.GetInteger("parent") == static_cast<int64_t>(parent));
  const Json& position = *item.GetObject("transform")->GetArray("position");
  CHECK(position.array_items()[2].number_value() == doctest::Approx(3.0));
  const Json& override_json =
      item.GetArray("material_overrides")->array_items()[0];
  CHECK(*override_json.GetString("material") == "paint");
  CHECK(override_json.GetNumber("roughness") == doctest::Approx(0.75));
  CHECK_FALSE(override_json.Find("metallic"));
  CHECK(json.GetObject("sun")->GetNumber("elevation_deg").has_value());
  CHECK(json.GetObject("environment")->Find("hdri")->is_null());
}

TEST_CASE("Prefab JSON describes materials, clips, sockets and physics") {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes.push_back({.name = "root"});
  prefab::Material material;
  material.name = "steel";
  material.metallic = 1.0f;
  prefab.materials.push_back(material);
  animation::Clip clip;
  clip.name = "open";
  clip.duration = 2.0f;
  clip.loop = true;
  clip.events.push_back({.name = "click", .time = 1.0f});
  prefab.clips.push_back(clip);
  prefab.sockets.push_back({.name = "muzzle", .node = 0});
  prefab.physics.kind = prefab::ColliderKind::kSphere;
  prefab.physics.radius = 0.4f;

  const Json json = PrefabToJson(prefab, "C:/assets");
  CHECK(json.GetObject("summary")->GetInteger("materials") == 1);
  const Json& steel = json.GetArray("materials")->array_items()[0];
  CHECK(*steel.GetString("name") == "steel");
  CHECK(steel.GetNumber("metallic") == doctest::Approx(1.0));
  CHECK(*steel.GetString("alpha_mode") == "opaque");
  const Json& open = json.GetArray("clips")->array_items()[0];
  CHECK(open.GetBool("loop") == true);
  CHECK(*open.GetArray("events")->array_items()[0].GetString("name") ==
        "click");
  CHECK(*json.GetArray("sockets")->array_items()[0].GetString("name") ==
        "muzzle");
  CHECK(*json.GetObject("physics")->GetString("collider") == "sphere");
  CHECK(json.GetObject("physics")->GetNumber("radius") == doctest::Approx(0.4));
  CHECK(json.Find("source_model")->is_null());
}

TEST_CASE("Screenshots are scaled to a maximum width and encoded as PNG") {
  RgbaImage image{.width = 4,
                  .height = 2,
                  .pixels = std::vector<std::byte>(4 * 2 * 4, std::byte{0})};
  // Left half white, right half black; alpha 0 (forced opaque).
  for (uint32_t y = 0; y < 2; ++y) {
    for (uint32_t x = 0; x < 2; ++x) {
      for (size_t c = 0; c < 3; ++c) {
        image.pixels[(y * 4 + x) * 4 + c] = std::byte{255};
      }
    }
  }
  const RgbaImage half = FitWidth(image, 2);
  REQUIRE(half.width == 2);
  CHECK(half.height == 1);
  CHECK(half.pixels[0] == std::byte{255});
  CHECK(half.pixels[4] == std::byte{0});
  CHECK(half.pixels[3] == std::byte{255});

  const RgbaImage same = FitWidth(image, 0);
  CHECK(same.width == 4);

  const std::vector<std::byte> png = EncodePng(half);
  REQUIRE(png.size() > 8);
  CHECK(png[1] == std::byte{'P'});
  CHECK(png[2] == std::byte{'N'});
  CHECK(png[3] == std::byte{'G'});
  CHECK(EncodePng(RgbaImage{}).empty());
}

}  // namespace
}  // namespace blue::editor
