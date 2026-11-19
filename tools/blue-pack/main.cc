// blue-pack: headless content cooker. Imports models into .pbo prefabs and
// prints what a prefab holds. No window, no GPU.
//
//   blue-pack import <model> -o <file.pbo> [options]
//   blue-pack info <file.pbo|file.mscn>
//   blue-pack samples --source <samples dir> --out <dir> [--fast]
//
// Exit codes: 0 success, 1 bad command line, 2 import or cook failure,
// 3 prefab file read/write failure.

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "asset/mesh_data.h"
#include "cook/clip_tools.h"
#include "cook/collision_cook.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "core/log.h"
#include "material/material.h"
#include "physics/physics_runtime.h"
#include "physics/shape_cooking.h"
#include "platform/file_system.h"
#include "platform/process_memory.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "tools/blue-pack/samples.h"

namespace blue::pack {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitCook = 2;
constexpr int kExitFile = 3;

constexpr std::string_view kUsage =
    "usage:\n"
    "  blue-pack import <model> -o <file.pbo> [options]\n"
    "  blue-pack info <file.pbo|file.mscn>\n"
    "  blue-pack samples --source <samples dir> --out <dir> [--fast]\n"
    "                              build the sample prefabs and range.mscn\n"
    "\n"
    "import options:\n"
    "  --scale <s>                 uniform scale (default 1)\n"
    "  --up-z                      source is Z-up (rotated to Y-up)\n"
    "  --merge-static              bake all nodes into one mesh (static "
    "models)\n"
    "  --drop-node <name>          remove a node and its subtree (repeatable)\n"
    "  --no-overrides              ignore sidecar .bmat material files\n"
    "  --no-optimize               skip vertex cache / fetch optimisation\n"
    "  --fast                      fast texture compression (iteration)\n"
    "  --uncompressed              keep textures as RGBA8\n"
    "  --max-texture-size <n>      halve textures until both sides <= n\n"
    "  --collider <kind>           none|box|sphere|capsule|hull|mesh\n"
    "                              (primitives fitted to the bounds)\n"
    "  --dynamic --mass <kg> --friction <f> --restitution <r>\n"
    "  --merge-clips <name>        merge every clip into one named clip\n"
    "  --rename-clip <old>=<new>   rename a clip\n"
    "  --trim-clip <clip>=<start>,<end>   cut a clip to [start, end] seconds\n"
    "  --loop <clip>               mark a clip as looping\n"
    "  --event <clip>=<name>@<time>       add a named event (seconds)\n"
    "  --socket <name>=<node>@<x>,<y>,<z>  add a socket ('-' = root node)\n";

template <typename... Args>
void Print(std::format_string<Args...> format, Args&&... args) {
  std::fputs(std::format(format, std::forward<Args>(args)...).c_str(), stdout);
}

template <typename T>
std::optional<T> ParseNumber(std::string_view text) {
  T value{};
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  if constexpr (std::is_floating_point_v<T>) {
    if (!std::isfinite(value)) {
      return std::nullopt;
    }
  }
  return value;
}

// Splits "a<separator>b" at the first separator.
std::optional<std::pair<std::string_view, std::string_view>> Split(
    std::string_view text, char separator) {
  const size_t at = text.find(separator);
  if (at == std::string_view::npos) {
    return std::nullopt;
  }
  return std::pair{text.substr(0, at), text.substr(at + 1)};
}

// Clip edits applied in command-line order after the import.
struct ClipEdit {
  enum class Kind { kMergeAll, kRename, kTrim, kLoop, kEvent };
  Kind kind = Kind::kLoop;
  std::string clip;
  std::string name;
  float start = 0.0f;
  float end = 0.0f;
};

struct SocketSpec {
  std::string name;
  std::string node;  // "-" = root
  glm::vec3 position{0.0f};
};

struct ImportCommand {
  std::string model;
  std::string output;
  cook::ModelCookOptions options;
  prefab::Physics physics;
  std::vector<ClipEdit> clip_edits;
  std::vector<SocketSpec> sockets;
};

std::optional<prefab::ColliderKind> ParseCollider(std::string_view text) {
  constexpr std::pair<std::string_view, prefab::ColliderKind> kNames[] = {
      {"none", prefab::ColliderKind::kNone},
      {"box", prefab::ColliderKind::kBox},
      {"sphere", prefab::ColliderKind::kSphere},
      {"capsule", prefab::ColliderKind::kCapsule},
      {"hull", prefab::ColliderKind::kConvexHull},
      {"mesh", prefab::ColliderKind::kTriangleMesh}};
  for (const auto& [name, kind] : kNames) {
    if (name == text) {
      return kind;
    }
  }
  return std::nullopt;
}

// Parses the arguments after "import". Logs the problem and returns nullopt
// on a bad command line.
std::optional<ImportCommand> ParseImport(
    std::span<const std::string_view> args) {
  ImportCommand command;
  const auto fail = [](std::string_view message, std::string_view arg) {
    core::log::Error("blue-pack import: {} '{}'", message, arg);
    return std::nullopt;
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const auto value = [&]() -> std::optional<std::string_view> {
      if (i + 1 >= args.size()) {
        return std::nullopt;
      }
      return args[++i];
    };
    if (!arg.starts_with("--") && arg != "-o") {
      if (!command.model.empty()) {
        return fail("unexpected argument", arg);
      }
      command.model = arg;
      continue;
    }
    if (arg == "--up-z") {
      command.options.up_axis = cook::UpAxis::kZ;
    } else if (arg == "--merge-static") {
      command.options.merge_static_nodes = true;
    } else if (arg == "--no-overrides") {
      command.options.use_material_overrides = false;
    } else if (arg == "--no-optimize") {
      command.options.optimize_meshes = false;
    } else if (arg == "--fast") {
      command.options.texture_quality = cook::CookQuality::kFast;
    } else if (arg == "--uncompressed") {
      command.options.compress_textures = false;
    } else if (arg == "--dynamic") {
      command.physics.dynamic = true;
    } else {
      const std::optional<std::string_view> v = value();
      if (!v.has_value()) {
        return fail("missing value after", arg);
      }
      if (arg == "-o") {
        command.output = *v;
      } else if (arg == "--scale") {
        const auto scale = ParseNumber<float>(*v);
        if (!scale.has_value() || *scale <= 0.0f) {
          return fail("invalid scale", *v);
        }
        command.options.scale = *scale;
      } else if (arg == "--drop-node") {
        command.options.drop_nodes.emplace_back(*v);
      } else if (arg == "--max-texture-size") {
        const auto size = ParseNumber<int>(*v);
        if (!size.has_value() || *size < 1) {
          return fail("invalid size", *v);
        }
        command.options.max_texture_size = *size;
      } else if (arg == "--collider") {
        const auto kind = ParseCollider(*v);
        if (!kind.has_value()) {
          return fail("unknown collider", *v);
        }
        command.physics.kind = *kind;
      } else if (arg == "--mass" || arg == "--friction" ||
                 arg == "--restitution") {
        const auto number = ParseNumber<float>(*v);
        if (!number.has_value() || *number < 0.0f) {
          return fail("invalid number", *v);
        }
        float& target = arg == "--mass"       ? command.physics.mass
                        : arg == "--friction" ? command.physics.friction
                                              : command.physics.restitution;
        target = *number;
      } else if (arg == "--merge-clips") {
        command.clip_edits.push_back(
            {.kind = ClipEdit::Kind::kMergeAll, .name = std::string(*v)});
      } else if (arg == "--loop") {
        command.clip_edits.push_back(
            {.kind = ClipEdit::Kind::kLoop, .clip = std::string(*v)});
      } else if (arg == "--rename-clip") {
        const auto parts = Split(*v, '=');
        if (!parts.has_value()) {
          return fail("expected <old>=<new>, got", *v);
        }
        command.clip_edits.push_back({.kind = ClipEdit::Kind::kRename,
                                      .clip = std::string(parts->first),
                                      .name = std::string(parts->second)});
      } else if (arg == "--trim-clip") {
        const auto parts = Split(*v, '=');
        const auto range =
            parts.has_value() ? Split(parts->second, ',') : std::nullopt;
        const auto start =
            range.has_value() ? ParseNumber<float>(range->first) : std::nullopt;
        const auto end = range.has_value() ? ParseNumber<float>(range->second)
                                           : std::nullopt;
        if (!start.has_value() || !end.has_value()) {
          return fail("expected <clip>=<start>,<end>, got", *v);
        }
        command.clip_edits.push_back({.kind = ClipEdit::Kind::kTrim,
                                      .clip = std::string(parts->first),
                                      .start = *start,
                                      .end = *end});
      } else if (arg == "--event") {
        const auto parts = Split(*v, '=');
        const auto event =
            parts.has_value() ? Split(parts->second, '@') : std::nullopt;
        const auto time = event.has_value() ? ParseNumber<float>(event->second)
                                            : std::nullopt;
        if (!time.has_value()) {
          return fail("expected <clip>=<name>@<time>, got", *v);
        }
        command.clip_edits.push_back({.kind = ClipEdit::Kind::kEvent,
                                      .clip = std::string(parts->first),
                                      .name = std::string(event->first),
                                      .start = *time});
      } else if (arg == "--socket") {
        const auto parts = Split(*v, '=');
        const auto place =
            parts.has_value() ? Split(parts->second, '@') : std::nullopt;
        const auto xy =
            place.has_value() ? Split(place->second, ',') : std::nullopt;
        const auto yz = xy.has_value() ? Split(xy->second, ',') : std::nullopt;
        const auto x =
            xy.has_value() ? ParseNumber<float>(xy->first) : std::nullopt;
        const auto y =
            yz.has_value() ? ParseNumber<float>(yz->first) : std::nullopt;
        const auto z =
            yz.has_value() ? ParseNumber<float>(yz->second) : std::nullopt;
        if (!x.has_value() || !y.has_value() || !z.has_value()) {
          return fail("expected <name>=<node>@<x>,<y>,<z>, got", *v);
        }
        command.sockets.push_back({.name = std::string(parts->first),
                                   .node = std::string(place->first),
                                   .position = {*x, *y, *z}});
      } else {
        return fail("unknown option", arg);
      }
    }
  }
  if (command.model.empty() || command.output.empty()) {
    core::log::Error("blue-pack import: need <model> and -o <file.pbo>");
    return std::nullopt;
  }
  return command;
}

// Applies `edit` to `prefab`; logs and returns false on failure.
bool ApplyClipEdit(prefab::Prefab& prefab, const ClipEdit& edit) {
  using Kind = ClipEdit::Kind;
  if (edit.kind == Kind::kMergeAll) {
    if (prefab.clips.empty()) {
      core::log::Error("blue-pack: --merge-clips: the model has no clips");
      return false;
    }
    std::vector<int> all(prefab.clips.size());
    for (size_t i = 0; i < all.size(); ++i) {
      all[i] = static_cast<int>(i);
    }
    return cook::MergeClips(prefab, all, edit.name).ok();
  }
  const int clip = prefab::FindClip(prefab, edit.clip);
  if (clip < 0) {
    core::log::Error("blue-pack: no clip '{}'", edit.clip);
    return false;
  }
  bool ok = false;
  switch (edit.kind) {
    case Kind::kRename:
      ok = cook::RenameClip(prefab, clip, edit.name).ok();
      break;
    case Kind::kTrim:
      ok = cook::TrimClip(prefab, clip, edit.start, edit.end).ok();
      break;
    case Kind::kLoop:
      ok = cook::SetClipLoop(prefab, clip, true).ok();
      break;
    case Kind::kEvent:
      ok = cook::AddClipEvent(prefab, clip, edit.name, edit.start).ok();
      break;
    case Kind::kMergeAll:
      break;
  }
  if (!ok) {
    core::log::Error("blue-pack: clip edit on '{}' failed (bad argument)",
                     edit.clip);
  }
  return ok;
}

// Prints the process's peak memory, where the platform reports it.
void PrintMemoryPeaks() {
  const platform::ProcessMemory memory = platform::QueryProcessMemory();
  if (!memory.available) {
    return;
  }
  constexpr double kMiB = 1024.0 * 1024.0;
  Print("peak memory: working set {:.0f} MiB, private {:.0f} MiB\n",
        static_cast<double>(memory.peak_working_set) / kMiB,
        static_cast<double>(memory.peak_private_bytes) / kMiB);
}

int RunImport(const ImportCommand& command) {
  const auto start = std::chrono::steady_clock::now();
  auto cooked = cook::CookModel(command.model, command.options);
  if (!cooked.ok()) {
    core::log::Error("blue-pack: cannot import {}: {}", command.model,
                     cook::ToString(cooked.error()));
    return kExitCook;
  }
  prefab::Prefab prefab = std::move(cooked).value();

  for (const ClipEdit& edit : command.clip_edits) {
    if (!ApplyClipEdit(prefab, edit)) {
      return kExitCook;
    }
  }
  for (const SocketSpec& socket : command.sockets) {
    int node = -1;
    if (socket.node != "-") {
      node = animation::FindNode(prefab.hierarchy, socket.node);
      if (node < 0) {
        core::log::Error("blue-pack: socket '{}': no node '{}'", socket.name,
                         socket.node);
        return kExitCook;
      }
    }
    prefab.sockets.push_back({.name = socket.name,
                              .node = node,
                              .local = {.translation = socket.position}});
  }
  if (command.physics.kind != prefab::ColliderKind::kNone) {
    physics::PhysicsRuntime runtime;
    if (auto collision = cook::CookCollision(prefab, command.physics);
        !collision.ok()) {
      core::log::Error("blue-pack: collision: {}",
                       cook::ToString(collision.error()));
      return kExitCook;
    }
  }

  if (auto saved = prefab::SavePrefab(prefab, command.output); !saved.ok()) {
    core::log::Error("blue-pack: cannot write {}: {}", command.output,
                     prefab::ToString(saved.error()));
    return saved.error() == prefab::PrefabError::kInvalidPrefab ? kExitCook
                                                                : kExitFile;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  Print("wrote {} in {:.2f} s\n", command.output, seconds);
  PrintMemoryPeaks();
  return kExitOk;
}

const char* FormatName(prefab::TextureFormat format) {
  switch (format) {
    case prefab::TextureFormat::kBc7Srgb:
      return "BC7 sRGB";
    case prefab::TextureFormat::kBc7Unorm:
      return "BC7 UNORM";
    case prefab::TextureFormat::kBc5Unorm:
      return "BC5 UNORM";
    case prefab::TextureFormat::kRgba8Srgb:
      return "RGBA8 sRGB";
    case prefab::TextureFormat::kRgba8Unorm:
      return "RGBA8 UNORM";
  }
  return "?";
}

const char* ColliderName(prefab::ColliderKind kind) {
  switch (kind) {
    case prefab::ColliderKind::kNone:
      return "none";
    case prefab::ColliderKind::kBox:
      return "box";
    case prefab::ColliderKind::kSphere:
      return "sphere";
    case prefab::ColliderKind::kCapsule:
      return "capsule";
    case prefab::ColliderKind::kConvexHull:
      return "convex hull";
    case prefab::ColliderKind::kTriangleMesh:
      return "triangle mesh";
  }
  return "?";
}

std::string Vec3(glm::vec3 v) {
  return std::format("({:.4g}, {:.4g}, {:.4g})", v.x, v.y, v.z);
}

std::string NodeName(const prefab::Prefab& prefab, int node) {
  if (node < 0 || static_cast<size_t>(node) >= prefab.hierarchy.nodes.size()) {
    return "<root>";
  }
  return std::format("{} '{}'", node,
                     prefab.hierarchy.nodes[static_cast<size_t>(node)].name);
}

void PrintPrefab(const prefab::Prefab& prefab) {
  Print("nodes: {}\n", prefab.hierarchy.nodes.size());
  for (size_t i = 0; i < prefab.hierarchy.nodes.size(); ++i) {
    const animation::Node& node = prefab.hierarchy.nodes[i];
    Print("  [{}] '{}' parent {} t {}\n", i, node.name, node.parent,
          Vec3(node.rest.translation));
  }

  size_t vertex_bytes = 0;
  size_t index_bytes = 0;
  Print("meshes: {}\n", prefab.meshes.size());
  for (const prefab::Mesh& mesh : prefab.meshes) {
    vertex_bytes += mesh.vertices.size_bytes();
    index_bytes += mesh.indices.size_bytes();
    Print("  '{}' node {}: {} vertices, {} triangles, bounds {} - {}\n",
          mesh.name, NodeName(prefab, mesh.node), mesh.vertices.size(),
          mesh.indices.size() / 3, Vec3(mesh.bounds.min),
          Vec3(mesh.bounds.max));
    for (const asset::Submesh& submesh : mesh.submeshes) {
      const std::string material =
          submesh.material >= 0 && static_cast<size_t>(submesh.material) <
                                       prefab.materials.size()
              ? prefab.materials[static_cast<size_t>(submesh.material)].name
              : "?";
      Print("    submesh {} triangles, material {} '{}'\n",
            submesh.index_count / 3, submesh.material, material);
    }
  }

  static constexpr const char* kSlotNames[] = {"base", "orm", "normal",
                                               "emissive"};
  Print("materials: {}\n", prefab.materials.size());
  for (const prefab::Material& material : prefab.materials) {
    Print(
        "  '{}' base ({:.3g}, {:.3g}, {:.3g}, {:.3g}) metallic {:.3g} "
        "roughness {:.3g} emissive {} x {:.3g}{}{}\n",
        material.name, material.base_color.r, material.base_color.g,
        material.base_color.b, material.base_color.a, material.metallic,
        material.roughness, Vec3(material.emissive), material.emissive_strength,
        material.alpha_mode == material::AlphaMode::kMask ? " mask" : "",
        material.double_sided ? " double-sided" : "");
    for (size_t slot = 0; slot < material.textures.size(); ++slot) {
      if (material.textures[slot] >= 0) {
        Print("    {}: texture {} (uv{})\n", kSlotNames[slot],
              material.textures[slot], material.uv_sets[slot]);
      }
    }
  }

  size_t texture_bytes = 0;
  Print("textures: {}\n", prefab.textures.size());
  for (size_t i = 0; i < prefab.textures.size(); ++i) {
    const prefab::Texture& texture = prefab.textures[i];
    size_t bytes = 0;
    for (const prefab::TextureMip& mip : texture.mips) {
      bytes += mip.data.size();
    }
    texture_bytes += bytes;
    Print("  [{}] '{}' {} {} x {}, {} mips, {} KiB\n", i, texture.name,
          FormatName(texture.format), texture.width, texture.height,
          texture.mip_count(), bytes / 1024);
    if (i < prefab.edit.texture_sources.size()) {
      for (const std::string& source : prefab.edit.texture_sources[i]) {
        Print("      from {}\n", source);
      }
    }
  }

  Print("clips: {}\n", prefab.clips.size());
  for (const animation::Clip& clip : prefab.clips) {
    Print("  '{}' {:.3f} s, {} channels{}\n", clip.name, clip.duration,
          clip.channels.size(), clip.loop ? ", loop" : "");
    for (const animation::ClipEvent& event : clip.events) {
      Print("    event '{}' at {:.3f} s\n", event.name, event.time);
    }
  }

  Print("sockets: {}\n", prefab.sockets.size());
  for (const prefab::Socket& socket : prefab.sockets) {
    Print("  '{}' node {} at {}\n", socket.name, NodeName(prefab, socket.node),
          Vec3(socket.local.translation));
  }
  Print("lights: {}\n", prefab.lights.size());
  for (const prefab::PointLight& light : prefab.lights) {
    Print("  '{}' node {} at {} colour {} intensity {:.3g} range {:.3g}\n",
          light.name, NodeName(prefab, light.node), Vec3(light.position),
          Vec3(light.color), light.intensity, light.range);
  }
  Print("children: {}\n", prefab.children.size());
  for (const prefab::ChildPrefab& child : prefab.children) {
    Print("  {} node {} at {}\n", child.path, NodeName(prefab, child.node),
          Vec3(child.local.translation));
  }

  const prefab::Physics& physics = prefab.physics;
  Print("physics: {}", ColliderName(physics.kind));
  if (physics.kind != prefab::ColliderKind::kNone) {
    Print(", {}{} kg, friction {:.3g}, restitution {:.3g}, cooked {} bytes",
          physics.dynamic ? "dynamic " : "static ",
          physics.dynamic ? std::format("{:.3g}", physics.mass) : "-",
          physics.friction, physics.restitution, physics.cooked_shape.size());
    if (physics.kind == prefab::ColliderKind::kBox) {
      Print(", half extents {} at {}", Vec3(physics.half_extents),
            Vec3(physics.center));
    } else if (physics.kind == prefab::ColliderKind::kSphere ||
               physics.kind == prefab::ColliderKind::kCapsule) {
      Print(", radius {:.4g} half height {:.4g} at {}", physics.radius,
            physics.half_height, Vec3(physics.center));
    }
  }
  Print("\n");

  Print("bounds: {} - {}\n", Vec3(prefab.bounds.min), Vec3(prefab.bounds.max));
  Print("source: {}\n", prefab.edit.source_model);
  if (!prefab.edit.import_settings.empty()) {
    Print("import settings:\n");
    std::string_view settings = prefab.edit.import_settings;
    while (!settings.empty()) {
      const size_t end = settings.find('\n');
      Print("  {}\n", settings.substr(0, end));
      settings = end == std::string_view::npos ? std::string_view()
                                               : settings.substr(end + 1);
    }
  }
  Print(
      "bulk: vertices {} KiB, indices {} KiB, textures {} KiB, collision {} "
      "KiB\n",
      vertex_bytes / 1024, index_bytes / 1024, texture_bytes / 1024,
      physics.cooked_shape.size() / 1024);
}

int RunInfo(const std::string& path) {
  const auto start = std::chrono::steady_clock::now();
  auto loaded = prefab::LoadPrefab(path, {.verify_bulk_checksum = true});
  const double verified_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
  if (!loaded.ok()) {
    core::log::Error("blue-pack: cannot load {}: {}", path,
                     prefab::ToString(loaded.error()));
    return kExitFile;
  }
  const auto fast_start = std::chrono::steady_clock::now();
  const bool fast_ok = prefab::LoadPrefab(path).ok();
  const double fast_ms = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - fast_start)
                             .count();
  const prefab::Prefab& prefab = loaded.value();
  Print("{}\n", path);
  PrintPrefab(prefab);
  size_t file_size = 0;
  if (auto bytes = platform::ReadFile(path, prefab::kMaxPrefabFileSize);
      bytes.ok()) {
    file_size = bytes.value().size();
  }
  Print(
      "file: {:.2f} MiB; load {:.1f} ms (default), {:.1f} ms (bulk CRC "
      "verified){}\n",
      static_cast<double>(file_size) / (1024.0 * 1024.0), fast_ms, verified_ms,
      fast_ok ? "" : " [default load FAILED]");

  if (!prefab.physics.cooked_shape.empty()) {
    physics::PhysicsRuntime runtime;
    auto shape = physics::CookedShape::Restore(prefab.physics.cooked_shape);
    if (shape.ok()) {
      const asset::Bounds bounds = shape.value().bounds();
      Print("collision shape restores: bounds {} - {}\n", Vec3(bounds.min),
            Vec3(bounds.max));
    } else {
      Print("collision shape does NOT restore: {}\n",
            physics::ToString(shape.error()));
    }
  }
  return kExitOk;
}

// Prints a summary of the scene at `path`: prefabs, instances (with
// overrides), lights, environment, player start.
int RunSceneInfo(const std::string& path) {
  const auto start = std::chrono::steady_clock::now();
  auto loaded = scene::LoadMscn(path);
  const double ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  if (!loaded.ok()) {
    core::log::Error("blue-pack: cannot load {}: {}", path,
                     scene::ToString(loaded.error()));
    return kExitFile;
  }
  const scene::SceneData& scene = loaded.value();
  Print("{}\n", path);
  Print("prefabs: {}\n", scene.prefabs.size());
  for (size_t i = 0; i < scene.prefabs.size(); ++i) {
    Print("  [{}] {}\n", i, scene.prefabs[i]);
  }
  Print("instances: {}\n", scene.instances.size());
  for (const scene::Instance& instance : scene.instances) {
    Print("  #{} '{}' prefab {} parent {} at {} scale {}{}\n", instance.id,
          instance.name, instance.prefab, instance.parent,
          Vec3(instance.transform.position), Vec3(instance.transform.scale),
          instance.visible ? "" : " hidden");
    for (const scene::MaterialOverride& material : instance.materials) {
      Print("    material '{}' <- prefab {} '{}'\n", material.material_name,
            material.source_prefab, material.source_material);
    }
  }
  Print("sun: direction {} colour {} intensity {:.3g}\n",
        Vec3(scene.sun.direction), Vec3(scene.sun.color), scene.sun.intensity);
  for (const scene::ScenePointLight& light : scene.point_lights) {
    Print("point light '{}' at {} colour {} intensity {:.3g} range {:.3g}\n",
          light.name, Vec3(light.position), Vec3(light.color), light.intensity,
          light.range);
  }
  Print("environment: {} intensity {:.3g} rotation {:.3g} exposure {:.3g} EV\n",
        scene.environment.hdri_path, scene.environment.intensity,
        scene.environment.rotation_radians, scene.environment.exposure_ev);
  Print("player start: {} yaw {:.3g} pitch {:.3g}\n",
        Vec3(scene.player_start.position), scene.player_start.yaw_radians,
        scene.player_start.pitch_radians);
  Print("gravity: {}\nload {:.1f} ms\n", Vec3(scene.gravity), ms);
  return kExitOk;
}

// Parses and runs "samples".
int RunSamples(std::span<const std::string_view> args) {
  SamplesOptions options;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--fast") {
      options.fast = true;
    } else if ((args[i] == "--out" || args[i] == "--source") &&
               i + 1 < args.size()) {
      std::string& target =
          args[i] == "--out" ? options.out_dir : options.source_dir;
      target = std::string(args[i + 1]);
      ++i;
    } else {
      core::log::Error("blue-pack samples: unexpected argument '{}'", args[i]);
      return kExitUsage;
    }
  }
  if (options.out_dir.empty() || options.source_dir.empty()) {
    core::log::Error("blue-pack samples: need --source <dir> and --out <dir>");
    return kExitUsage;
  }
  return CookSamples(options) ? kExitOk : kExitCook;
}

int Run(std::span<const std::string_view> args) {
  if (args.empty() || args[0] == "help" || args[0] == "--help") {
    std::fputs(kUsage.data(), args.empty() ? stderr : stdout);
    return args.empty() ? kExitUsage : kExitOk;
  }
  if (args[0] == "import") {
    const std::optional<ImportCommand> command = ParseImport(args.subspan(1));
    if (!command.has_value()) {
      std::fputs(kUsage.data(), stderr);
      return kExitUsage;
    }
    return RunImport(*command);
  }
  if (args[0] == "info") {
    if (args.size() != 2) {
      std::fputs(kUsage.data(), stderr);
      return kExitUsage;
    }
    const std::string path(args[1]);
    return path.ends_with(".mscn") ? RunSceneInfo(path) : RunInfo(path);
  }
  if (args[0] == "samples") {
    return RunSamples(args.subspan(1));
  }
  core::log::Error("blue-pack: unknown command '{}'", args[0]);
  std::fputs(kUsage.data(), stderr);
  return kExitUsage;
}

}  // namespace
}  // namespace blue::pack

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return blue::pack::Run(args);
}
