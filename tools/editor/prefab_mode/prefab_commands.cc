#include "editor/prefab_mode/prefab_commands.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "cook/clip_tools.h"
#include "cook/collision_cook.h"
#include "cook/cook_error.h"
#include "cook/model_cook.h"
#include "cook/texture_cook.h"
#include "core/log.h"
#include "core/result.h"
#include "editor/commands.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "world/prefab_pose.h"

namespace blue::editor {

namespace {

using Status = core::Result<void, std::string>;
using Args = std::span<const std::string>;

[[nodiscard]] Status Fail(std::string message) {
  return Status::Error(std::move(message));
}

[[nodiscard]] Status RequireArgs(Args args, size_t count,
                                 std::string_view usage) {
  if (args.size() < count) {
    return Fail(std::format("usage: {}", usage));
  }
  return Status::Ok();
}

// Parses `args[first..first+N)` as floats into `out`.
template <size_t N>
[[nodiscard]] Status ParseFloats(Args args, size_t first,
                                 std::array<float, N>& out,
                                 std::string_view what) {
  if (args.size() < first + N) {
    return Fail(std::format("{} needs {} numbers", what, N));
  }
  for (size_t i = 0; i < N; ++i) {
    core::Result<float, std::string> value =
        ParseFloatArg(args[first + i], what);
    if (!value.ok()) {
      return Fail(value.error());
    }
    out[i] = value.value();
  }
  return Status::Ok();
}

[[nodiscard]] std::optional<SelectionKind> ParseKind(std::string_view word) {
  static constexpr std::array<std::pair<std::string_view, SelectionKind>, 9>
      kKinds{{{"node", SelectionKind::kNode},
              {"mesh", SelectionKind::kMesh},
              {"material", SelectionKind::kMaterial},
              {"texture", SelectionKind::kTexture},
              {"socket", SelectionKind::kSocket},
              {"light", SelectionKind::kLight},
              {"child", SelectionKind::kChild},
              {"clip", SelectionKind::kClip},
              {"collider", SelectionKind::kCollider}}};
  for (const auto& [name, kind] : kKinds) {
    if (name == word) {
      return kind;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<prefab::TextureSlot> ParseSlot(
    std::string_view word) {
  if (word == "base") {
    return prefab::TextureSlot::kBaseColor;
  }
  if (word == "orm") {
    return prefab::TextureSlot::kOrm;
  }
  if (word == "normal") {
    return prefab::TextureSlot::kNormal;
  }
  if (word == "emissive") {
    return prefab::TextureSlot::kEmissive;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<prefab::ColliderKind> ParseCollider(
    std::string_view word) {
  static constexpr std::array<std::string_view, 6> kNames{
      "none", "box", "sphere", "capsule", "hull", "mesh"};
  for (size_t i = 0; i < kNames.size(); ++i) {
    if (kNames[i] == word) {
      return static_cast<prefab::ColliderKind>(i);
    }
  }
  return std::nullopt;
}

// Parses "<kind> <index>" (the index may be a name for named kinds) from
// args[0..1] into a selection item of the document's prefab.
[[nodiscard]] core::Result<SelectionItem, std::string> ParseItem(
    const prefab::Prefab& prefab, Args args) {
  using Result = core::Result<SelectionItem, std::string>;
  if (args.empty()) {
    return Result::Error("expected <kind> <index|name>");
  }
  const std::optional<SelectionKind> kind = ParseKind(args[0]);
  if (!kind) {
    return Result::Error("unknown kind '" + args[0] +
                         "' (node mesh material texture socket light child "
                         "clip collider)");
  }
  if (*kind == SelectionKind::kCollider) {
    return Result::Ok({.kind = *kind, .index = -1});
  }
  if (args.size() < 2) {
    return Result::Error("expected an index or name after " + args[0]);
  }
  core::Result<int, std::string> index = ParseIntArg(args[1], "index");
  SelectionItem item{.kind = *kind, .index = -1};
  if (index.ok()) {
    item.index = index.value();
  } else {
    // By name: the first item whose name matches.
    for (int64_t i = 0; i < 100000; ++i) {
      item.index = i;
      const std::optional<std::string> name = ItemName(prefab, item);
      if (!name) {
        break;
      }
      if (*name == args[1]) {
        return Result::Ok(item);
      }
    }
    return Result::Error("no " + args[0] + " called '" + args[1] + "'");
  }
  if (ItemLabel(prefab, item).empty()) {
    return Result::Error(std::format("no {} {}", args[0], item.index));
  }
  return Result::Ok(item);
}

// Applies one material parameter from `args` (after the material index).
[[nodiscard]] Status SetMaterialParam(prefab::Material& material, Args args) {
  const std::string& name = args[0];
  const auto number = [&](float& out) -> Status {
    std::array<float, 1> value{};
    Status parsed = ParseFloats(args, 1, value, name);
    if (parsed.ok()) {
      out = value[0];
    }
    return parsed;
  };
  if (name == "base_color") {
    std::array<float, 4> c{0.0f, 0.0f, 0.0f, 1.0f};
    if (args.size() == 4) {
      std::array<float, 3> rgb{};
      Status parsed = ParseFloats(args, 1, rgb, name);
      if (!parsed.ok()) {
        return parsed;
      }
      material.base_color = glm::vec4(rgb[0], rgb[1], rgb[2], 1.0f);
      return Status::Ok();
    }
    Status parsed = ParseFloats(args, 1, c, name);
    if (parsed.ok()) {
      material.base_color = glm::vec4(c[0], c[1], c[2], c[3]);
    }
    return parsed;
  }
  if (name == "emissive") {
    std::array<float, 3> c{};
    Status parsed = ParseFloats(args, 1, c, name);
    if (parsed.ok()) {
      material.emissive = glm::vec3(c[0], c[1], c[2]);
    }
    return parsed;
  }
  if (name == "uv_scale") {
    std::array<float, 2> uv{};
    Status parsed = ParseFloats(args, 1, uv, name);
    if (parsed.ok()) {
      material.uv_scale = {uv[0], uv[1]};
    }
    return parsed;
  }
  if (name == "metallic") return number(material.metallic);
  if (name == "roughness") return number(material.roughness);
  if (name == "emissive_strength") return number(material.emissive_strength);
  if (name == "normal_scale") return number(material.normal_scale);
  if (name == "occlusion_strength") return number(material.occlusion_strength);
  if (name == "alpha_cutoff") return number(material.alpha_cutoff);
  if (name == "alpha" && args.size() >= 2) {
    if (args[1] != "opaque" && args[1] != "mask") {
      return Fail("alpha is opaque or mask");
    }
    material.alpha_mode = args[1] == "mask" ? material::AlphaMode::kMask
                                            : material::AlphaMode::kOpaque;
    return Status::Ok();
  }
  if (name == "double_sided" && args.size() >= 2) {
    material.double_sided = args[1] == "1" || args[1] == "true";
    return Status::Ok();
  }
  if (name == "uv_set" && args.size() >= 3) {
    const std::optional<prefab::TextureSlot> slot = ParseSlot(args[1]);
    if (!slot || (args[2] != "0" && args[2] != "1")) {
      return Fail("usage: uv_set <base|orm|normal|emissive> <0|1>");
    }
    material.uv_sets[static_cast<size_t>(*slot)] = args[2] == "1" ? 1 : 0;
    return Status::Ok();
  }
  return Fail("unknown parameter '" + name +
              "' (base_color metallic roughness emissive emissive_strength "
              "normal_scale occlusion_strength uv_scale alpha alpha_cutoff "
              "double_sided uv_set)");
}

[[nodiscard]] Status FromCook(core::Result<void, cook::CookError> result,
                              std::string_view what) {
  if (!result.ok()) {
    return Fail(std::format("{}: {}", what, CookErrorText(result.error())));
  }
  return Status::Ok();
}

// Wraps `op` (returning core::Result<..., cook::CookError>) for TryEditQuiet.
template <typename Op>
[[nodiscard]] auto CookOp(std::string_view what, Op op) {
  return [what, op = std::move(op)](
             prefab::Prefab& prefab) mutable -> std::optional<std::string> {
    const auto result = op(prefab);
    if (!result.ok()) {
      return std::format("{}: {}", what, CookErrorText(result.error()));
    }
    return std::nullopt;
  };
}

}  // namespace

void RegisterPrefabCommands(EditorContext& context, PrefabModeShared& shared) {
  CommandRegistry& commands = context.commands();
  EditorContext* ctx = &context;
  PrefabModeShared* state = &shared;
  // Returns the open prefab document or nullptr.
  const auto doc = [ctx]() { return ctx->prefab_document(); };
  const auto no_doc = [] { return Fail("no prefab document is open"); };

  commands.Register(
      "prefab-import",
      "prefab-import <model> [scale <s>] [zup] [merge] [fast] [drop <node>]: "
      "cooks a model (in the background) into a new prefab document",
      [ctx, state](Args args) -> Status {
        if (Status ok = RequireArgs(args, 1, "prefab-import <model> ...");
            !ok.ok()) {
          return ok;
        }
        RequestImport(*state, args[0]);
        state->import_dialog.open_requested = false;
        cook::ModelCookOptions& options = state->import_dialog.options;
        options = {};
        state->import_dialog.drop_nodes.clear();
        for (size_t i = 1; i < args.size(); ++i) {
          if (args[i] == "scale" && i + 1 < args.size()) {
            core::Result<float, std::string> scale =
                ParseFloatArg(args[++i], "scale");
            if (!scale.ok()) {
              return Fail(scale.error());
            }
            options.scale = scale.value();
          } else if (args[i] == "zup") {
            options.up_axis = cook::UpAxis::kZ;
          } else if (args[i] == "merge") {
            options.merge_static_nodes = true;
          } else if (args[i] == "fast") {
            options.texture_quality = cook::CookQuality::kFast;
          } else if (args[i] == "drop" && i + 1 < args.size()) {
            std::string& nodes = state->import_dialog.drop_nodes;
            nodes += (nodes.empty() ? "" : ",") + args[++i];
          } else {
            return Fail("unknown option '" + args[i] + "'");
          }
        }
        StartImportCook(*ctx, *state);
        return Status::Ok();
      });

  commands.Register("prefab-reimport",
                    "prefab-reimport: re-imports the source model with the "
                    "stored options, keeping the edits",
                    [ctx, state, doc, no_doc](Args) -> Status {
                      PrefabDocument* document = doc();
                      if (document == nullptr) {
                        return no_doc();
                      }
                      if (document->state().edit.source_model.empty()) {
                        return Fail("no source model recorded");
                      }
                      RequestReimport(*ctx, *state);
                      state->import_dialog.open_requested = false;
                      StartImportCook(*ctx, *state);
                      return Status::Ok();
                    });

  commands.Register(
      "prefab-select",
      "prefab-select <kind> <index|name> | none: selects an item (node mesh "
      "material texture socket light child clip collider)",
      [doc, no_doc, state](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (!args.empty() && args[0] == "none") {
          document->selection().Clear();
          return Status::Ok();
        }
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        document->selection().Set(item.value());
        if (item.value().kind == SelectionKind::kClip) {
          state->preview.clip = static_cast<int>(item.value().index);
        }
        return Status::Ok();
      });

  commands.Register(
      "prefab-rename", "prefab-rename <kind> <index|name> <new name>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: prefab-rename <kind> <index> <name>");
        }
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        return TryEditQuiet(*document, "Rename", ChangeKind::kStructure,
                            [&](prefab::Prefab& prefab) {
                              return RenameItem(prefab, item.value(), args[2]);
                            });
      });

  commands.Register("prefab-delete", "prefab-delete <kind> <index|name>",
                    [doc, no_doc](Args args) -> Status {
                      PrefabDocument* document = doc();
                      if (document == nullptr) {
                        return no_doc();
                      }
                      core::Result<SelectionItem, std::string> item =
                          ParseItem(document->state(), args);
                      if (!item.ok()) {
                        return Fail(item.error());
                      }
                      document->selection().Clear();
                      return TryEditQuiet(
                          *document, "Delete", ChangeKind::kStructure,
                          [&](prefab::Prefab& prefab) {
                            return DeleteItem(prefab, item.value());
                          });
                    });

  commands.Register(
      "material-set",
      "material-set <material index|name> <param> <values...>: base_color r g "
      "b [a], metallic, roughness, emissive r g b, emissive_strength, "
      "normal_scale, occlusion_strength, uv_scale u v, alpha opaque|mask, "
      "alpha_cutoff, double_sided 0|1, uv_set <slot> 0|1",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: material-set <material> <param> <values...>");
        }
        const std::array<std::string, 2> item_args{"material", args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        const size_t index = static_cast<size_t>(item.value().index);
        prefab::Material material = document->state().materials[index];
        if (Status set = SetMaterialParam(material, args.subspan(1));
            !set.ok()) {
          return set;
        }
        document->Edit("Edit material", ChangeKind::kParameters,
                       [&](prefab::Prefab& prefab) {
                         prefab.materials[index] = std::move(material);
                       });
        document->selection().Set(item.value());
        return Status::Ok();
      });

  commands.Register(
      "texture-import",
      "texture-import <material> <base|orm|normal|emissive> <image> [fast]: "
      "cooks an image into a material slot",
      [doc, no_doc, state](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: texture-import <material> <slot> <image>");
        }
        const std::array<std::string, 2> item_args{"material", args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        const std::optional<prefab::TextureSlot> slot = ParseSlot(args[1]);
        if (!item.ok() || !slot) {
          return Fail(item.ok() ? "bad slot" : item.error());
        }
        TextureImportSettings settings = state->texture_settings;
        if (args.size() > 3 && args[3] == "fast") {
          settings.quality = cook::CookQuality::kFast;
        }
        const int material = static_cast<int>(item.value().index);
        return TryEditQuiet(
            *document, "Replace texture", ChangeKind::kStructure,
            CookOp("texture-import", [&](prefab::Prefab& prefab) {
              return ImportSlotTexture(prefab, material, *slot, args[2],
                                       settings);
            }));
      });

  commands.Register(
      "texture-pack-orm",
      "texture-pack-orm <material> <occlusion image|-> <ch> <roughness "
      "image|-> <ch> <metallic image|-> <ch>: packs and cooks an ORM texture",
      [doc, no_doc, state](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 7) {
          return Fail(
              "usage: texture-pack-orm <material> <img> <ch> <img> "
              "<ch> <img> <ch>");
        }
        const std::array<std::string, 2> item_args{"material", args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        OrmImportSources sources;
        const std::array<OrmChannelSource*, 3> channels{
            &sources.occlusion, &sources.roughness, &sources.metallic};
        for (size_t i = 0; i < 3; ++i) {
          const std::string& path = args[1 + i * 2];
          core::Result<int, std::string> channel =
              ParseIntArg(args[2 + i * 2], "channel");
          if (!channel.ok()) {
            return Fail(channel.error());
          }
          *channels[i] = {.path = path == "-" ? std::string() : path,
                          .channel = channel.value()};
        }
        const int material = static_cast<int>(item.value().index);
        return TryEditQuiet(
            *document, "Pack ORM texture", ChangeKind::kStructure,
            CookOp("texture-pack-orm", [&](prefab::Prefab& prefab) {
              return ImportOrmTexture(prefab, material, sources,
                                      state->texture_settings);
            }));
      });

  commands.Register(
      "texture-remove-unused",
      "texture-remove-unused: drops textures no slot uses",
      [doc, no_doc](Args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        return TryEditQuiet(
            *document, "Remove unused textures", ChangeKind::kStructure,
            [](prefab::Prefab& prefab) -> std::optional<std::string> {
              (void)RemoveUnusedTextures(prefab);
              return std::nullopt;
            });
      });

  commands.Register(
      "submesh-material", "submesh-material <mesh> <submesh> <material>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: submesh-material <mesh> <submesh> <material>");
        }
        std::array<int, 3> values{};
        for (size_t i = 0; i < 3; ++i) {
          core::Result<int, std::string> v = ParseIntArg(args[i], "index");
          if (!v.ok()) {
            return Fail(v.error());
          }
          values[i] = v.value();
        }
        return TryEditQuiet(
            *document, "Assign material", ChangeKind::kStructure,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              if (!SetSubmeshMaterial(prefab, values[0], values[1],
                                      values[2])) {
                return std::string("bad mesh, submesh or material index");
              }
              return std::nullopt;
            });
      });

  commands.Register(
      "node-reparent", "node-reparent <node> <new parent node|-1>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 2) {
          return Fail("usage: node-reparent <node> <parent|-1>");
        }
        core::Result<int, std::string> node = ParseIntArg(args[0], "node");
        core::Result<int, std::string> parent = ParseIntArg(args[1], "parent");
        if (!node.ok() || !parent.ok()) {
          return Fail("node and parent are indices");
        }
        document->selection().Clear();
        return TryEditQuiet(
            *document, "Reparent node", ChangeKind::kStructure,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              if (!ReparentNode(prefab, node.value(), parent.value())) {
                return std::string("bad index or parent inside the subtree");
              }
              return std::nullopt;
            });
      });

  commands.Register(
      "socket-add",
      "socket-add <name> [node] [x y z]: adds a socket (node -1 = root)",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.empty()) {
          return Fail("usage: socket-add <name> [node] [x y z]");
        }
        int node = -1;
        if (args.size() >= 2) {
          core::Result<int, std::string> parsed = ParseIntArg(args[1], "node");
          if (!parsed.ok()) {
            return Fail(parsed.error());
          }
          node = parsed.value();
        }
        std::array<float, 3> position{};
        if (args.size() >= 5) {
          if (Status ok = ParseFloats(args, 2, position, "position");
              !ok.ok()) {
            return ok;
          }
        }
        int added = -1;
        Status edited = TryEditQuiet(
            *document, "Add socket", ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              if (node < -1 ||
                  node >= static_cast<int>(prefab.hierarchy.nodes.size())) {
                return std::string("no such node");
              }
              added = AddSocket(prefab, node, args[0]);
              prefab.sockets[static_cast<size_t>(added)].local.translation = {
                  position[0], position[1], position[2]};
              return std::nullopt;
            });
        if (edited.ok()) {
          document->selection().Set(
              {.kind = SelectionKind::kSocket, .index = added});
        }
        return edited;
      });

  commands.Register(
      "socket-set",
      "socket-set <socket index|name> <x> <y> <z> [rx ry rz degrees]",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        const std::array<std::string, 2> item_args{
            "socket", args.empty() ? std::string() : args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        std::array<float, 3> position{};
        if (Status ok = ParseFloats(args, 1, position, "position"); !ok.ok()) {
          return ok;
        }
        const size_t i = static_cast<size_t>(item.value().index);
        animation::Transform local = document->state().sockets[i].local;
        local.translation = {position[0], position[1], position[2]};
        if (args.size() >= 7) {
          std::array<float, 3> degrees{};
          if (Status ok = ParseFloats(args, 4, degrees, "rotation"); !ok.ok()) {
            return ok;
          }
          local.rotation = glm::quat(
              glm::radians(glm::vec3(degrees[0], degrees[1], degrees[2])));
        }
        document->Edit(
            "Move socket", ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) { prefab.sockets[i].local = local; });
        return Status::Ok();
      });

  commands.Register(
      "light-add", "light-add [node] [x y z]: adds a point light",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        int node = -1;
        if (!args.empty()) {
          core::Result<int, std::string> parsed = ParseIntArg(args[0], "node");
          if (!parsed.ok()) {
            return Fail(parsed.error());
          }
          node = parsed.value();
        }
        std::array<float, 3> position{};
        if (args.size() >= 4) {
          if (Status ok = ParseFloats(args, 1, position, "position");
              !ok.ok()) {
            return ok;
          }
        }
        int added = -1;
        Status edited = TryEditQuiet(
            *document, "Add light", ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) -> std::optional<std::string> {
              if (node < -1 ||
                  node >= static_cast<int>(prefab.hierarchy.nodes.size())) {
                return std::string("no such node");
              }
              added = AddPointLight(prefab, node);
              prefab.lights[static_cast<size_t>(added)].position = {
                  position[0], position[1], position[2]};
              return std::nullopt;
            });
        if (edited.ok()) {
          document->selection().Set(
              {.kind = SelectionKind::kLight, .index = added});
        }
        return edited;
      });

  commands.Register(
      "light-set",
      "light-set <light index|name> <position x y z | color r g b | "
      "intensity v | range m>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: light-set <light> <param> <values...>");
        }
        const std::array<std::string, 2> item_args{"light", args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        const size_t i = static_cast<size_t>(item.value().index);
        prefab::PointLight light = document->state().lights[i];
        const std::string& param = args[1];
        if (param == "position" || param == "color") {
          std::array<float, 3> v{};
          if (Status ok = ParseFloats(args, 2, v, param); !ok.ok()) {
            return ok;
          }
          (param == "position" ? light.position : light.color) =
              glm::vec3(v[0], v[1], v[2]);
        } else if (param == "intensity" || param == "range") {
          std::array<float, 1> v{};
          if (Status ok = ParseFloats(args, 2, v, param); !ok.ok()) {
            return ok;
          }
          (param == "intensity" ? light.intensity : light.range) = v[0];
        } else {
          return Fail("unknown light parameter '" + param + "'");
        }
        document->Edit(
            "Edit light", ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) { prefab.lights[i] = light; });
        return Status::Ok();
      });

  commands.Register(
      "child-add",
      "child-add <prefab.pbo> [node]: adds a child prefab (cycle-checked)",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.empty()) {
          return Fail("usage: child-add <prefab.pbo> [node]");
        }
        int node = -1;
        if (args.size() >= 2) {
          core::Result<int, std::string> parsed = ParseIntArg(args[1], "node");
          if (!parsed.ok()) {
            return Fail(parsed.error());
          }
          node = parsed.value();
        }
        return TryEditQuiet(
            *document, "Add child prefab", ChangeKind::kStructure,
            [&](prefab::Prefab& prefab) {
              return AddChildPrefab(prefab, document->path(), args[0], node);
            });
      });

  commands.Register(
      "collider",
      "collider <none|box|sphere|capsule|hull|mesh>: sets the collider, "
      "fitted to the bounds, and cooks it",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        const std::optional<prefab::ColliderKind> kind =
            args.empty() ? std::nullopt : ParseCollider(args[0]);
        if (!kind) {
          return Fail("usage: collider <none|box|sphere|capsule|hull|mesh>");
        }
        return TryEditQuiet(
            *document, "Collider kind", ChangeKind::kParameters,
            CookOp("collider", [&](prefab::Prefab& prefab) {
              prefab::Physics settings = prefab.physics;
              settings.kind = *kind;
              if (*kind == prefab::ColliderKind::kTriangleMesh) {
                settings.dynamic = false;
              }
              return cook::CookCollision(prefab, settings,
                                         cook::ShapeFit::kBounds);
            }));
      });

  commands.Register(
      "body",
      "body <dynamic 0|1 | mass kg | friction f | restitution r>: sets a "
      "body parameter",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 2) {
          return Fail("usage: body <param> <value>");
        }
        prefab::Physics physics = document->state().physics;
        if (args[0] == "dynamic") {
          physics.dynamic = args[1] == "1" || args[1] == "true";
        } else {
          core::Result<float, std::string> value =
              ParseFloatArg(args[1], args[0]);
          if (!value.ok()) {
            return Fail(value.error());
          }
          if (args[0] == "mass") {
            physics.mass = value.value();
          } else if (args[0] == "friction") {
            physics.friction = value.value();
          } else if (args[0] == "restitution") {
            physics.restitution = value.value();
          } else {
            return Fail("unknown body parameter '" + args[0] + "'");
          }
        }
        document->Edit(
            "Edit physics", ChangeKind::kParameters,
            [&](prefab::Prefab& prefab) { prefab.physics = physics; });
        return Status::Ok();
      });

  commands.Register(
      "clip-play",
      "clip-play <clip index|name> [speed]: plays the clip on the preview",
      [ctx, doc, no_doc, state](Args args) -> Status {
        PrefabDocument* document = doc();
        world::PrefabPose* pose = ctx->prefab_pose();
        if (document == nullptr || pose == nullptr) {
          return no_doc();
        }
        const std::array<std::string, 2> item_args{
            "clip", args.empty() ? std::string("0") : args[0]};
        core::Result<SelectionItem, std::string> item =
            ParseItem(document->state(), item_args);
        if (!item.ok()) {
          return Fail(item.error());
        }
        AnimationPreview& preview = state->preview;
        preview.clip = static_cast<int>(item.value().index);
        preview.time = 0.0f;
        preview.speed = 1.0f;
        if (args.size() >= 2) {
          core::Result<float, std::string> speed =
              ParseFloatArg(args[1], "speed");
          if (!speed.ok()) {
            return Fail(speed.error());
          }
          preview.speed = speed.value();
        }
        preview.loop =
            document->state().clips[static_cast<size_t>(preview.clip)].loop;
        preview.playing = true;
        pose->Play(preview.clip, 0.0f, preview.speed, preview.loop);
        return Status::Ok();
      });

  commands.Register("clip-stop", "clip-stop: pauses the preview",
                    [ctx, state](Args) -> Status {
                      state->preview.playing = false;
                      if (world::PrefabPose* pose = ctx->prefab_pose()) {
                        pose->Stop();
                      }
                      return Status::Ok();
                    });

  commands.Register(
      "clip-time", "clip-time <seconds>: shows the current clip at a time",
      [ctx, state](Args args) -> Status {
        if (args.empty()) {
          return Fail("usage: clip-time <seconds>");
        }
        core::Result<float, std::string> time = ParseFloatArg(args[0], "time");
        if (!time.ok()) {
          return Fail(time.error());
        }
        world::PrefabPose* pose = ctx->prefab_pose();
        if (pose == nullptr || state->preview.clip < 0) {
          return Fail("no clip selected");
        }
        state->preview.playing = false;
        state->preview.time = time.value();
        pose->Play(state->preview.clip, time.value(), 0.0f);
        pose->Stop();
        pose->SetTime(time.value());
        return Status::Ok();
      });

  commands.Register(
      "clip-merge", "clip-merge <new name> <clip> <clip>...",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: clip-merge <name> <i> <j>...");
        }
        std::vector<int> clips;
        for (size_t i = 1; i < args.size(); ++i) {
          core::Result<int, std::string> clip = ParseIntArg(args[i], "clip");
          if (!clip.ok()) {
            return Fail(clip.error());
          }
          clips.push_back(clip.value());
        }
        return TryEditQuiet(*document, "Merge clips", ChangeKind::kStructure,
                            CookOp("clip-merge", [&](prefab::Prefab& prefab) {
                              return cook::MergeClips(prefab, clips, args[0]);
                            }));
      });

  commands.Register(
      "clip-trim", "clip-trim <clip> <start s> <end s>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: clip-trim <clip> <start> <end>");
        }
        core::Result<int, std::string> clip = ParseIntArg(args[0], "clip");
        std::array<float, 2> range{};
        if (!clip.ok()) {
          return Fail(clip.error());
        }
        if (Status ok = ParseFloats(args, 1, range, "range"); !ok.ok()) {
          return ok;
        }
        return TryEditQuiet(*document, "Trim clip", ChangeKind::kStructure,
                            CookOp("clip-trim", [&](prefab::Prefab& prefab) {
                              return cook::TrimClip(prefab, clip.value(),
                                                    range[0], range[1]);
                            }));
      });

  commands.Register(
      "clip-loop", "clip-loop <clip> <0|1>: sets the authored loop flag",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 2) {
          return Fail("usage: clip-loop <clip> <0|1>");
        }
        core::Result<int, std::string> clip = ParseIntArg(args[0], "clip");
        if (!clip.ok()) {
          return Fail(clip.error());
        }
        const bool loop = args[1] == "1" || args[1] == "true";
        return TryEditQuiet(*document, "Clip loop", ChangeKind::kStructure,
                            CookOp("clip-loop", [&](prefab::Prefab& prefab) {
                              return cook::SetClipLoop(prefab, clip.value(),
                                                       loop);
                            }));
      });

  commands.Register(
      "event-add", "event-add <clip> <name> <time s>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 3) {
          return Fail("usage: event-add <clip> <name> <time>");
        }
        core::Result<int, std::string> clip = ParseIntArg(args[0], "clip");
        core::Result<float, std::string> time = ParseFloatArg(args[2], "time");
        if (!clip.ok() || !time.ok()) {
          return Fail("clip is an index, time in seconds");
        }
        return TryEditQuiet(*document, "Add event", ChangeKind::kStructure,
                            CookOp("event-add", [&](prefab::Prefab& prefab) {
                              return cook::AddClipEvent(prefab, clip.value(),
                                                        args[1], time.value());
                            }));
      });

  commands.Register(
      "event-remove", "event-remove <clip> <event index>",
      [doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (args.size() < 2) {
          return Fail("usage: event-remove <clip> <event>");
        }
        core::Result<int, std::string> clip = ParseIntArg(args[0], "clip");
        core::Result<int, std::string> event = ParseIntArg(args[1], "event");
        if (!clip.ok() || !event.ok()) {
          return Fail("clip and event are indices");
        }
        return TryEditQuiet(*document, "Delete event", ChangeKind::kStructure,
                            CookOp("event-remove", [&](prefab::Prefab& prefab) {
                              return cook::RemoveClipEvent(prefab, clip.value(),
                                                           event.value());
                            }));
      });

  commands.Register(
      "prefab-validate",
      "prefab-validate: fails with the reason the prefab cannot be saved",
      [ctx, doc, no_doc](Args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        if (std::optional<std::string> problem =
                CheckBeforeSave(document->state())) {
          return Fail(*problem);
        }
        ctx->Status("Prefab is valid");
        return Status::Ok();
      });

  commands.Register(
      "prefab-info", "prefab-info: logs the prefab's counts and sizes",
      [ctx, doc, no_doc](Args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        const PrefabSummary s = Summarize(document->state());
        const std::string text = std::format(
            "{}: {} nodes, {} meshes ({} verts, {} tris), {} materials, {} "
            "textures ({} KiB), {} clips, {} sockets, {} lights, {} children, "
            "collision {} B",
            DocumentTitle(document->path()), s.node_count, s.mesh_count,
            s.vertex_count, s.triangle_count, s.material_count, s.texture_count,
            s.texture_bytes / 1024, s.clip_count, s.socket_count, s.light_count,
            s.child_count, s.collision_bytes);
        core::log::Info("{}", text);
        ctx->Status(text);
        return Status::Ok();
      });

  commands.Register(
      "prefab-save",
      "prefab-save [path.pbo]: validates and saves the prefab document",
      [ctx, doc, no_doc](Args args) -> Status {
        PrefabDocument* document = doc();
        if (document == nullptr) {
          return no_doc();
        }
        const std::string path = args.empty() ? std::string() : args[0];
        if (path.empty() && document->path().empty()) {
          return Fail("the document is untitled: give a path");
        }
        Status saved = SavePrefabChecked(*document, path);
        if (saved.ok()) {
          ctx->Status("Saved " + document->path());
        }
        return saved;
      });
}

}  // namespace blue::editor
