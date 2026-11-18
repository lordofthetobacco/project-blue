// Scene mode's text commands (--exec scripts, the supervisor's way to drive
// the editor without a mouse). Every command acts on the open scene document
// as one undo step and fails with a readable message.
//
//   scene-add <file.pbo> [x y z]          place an instance (default: on the
//                                         ground in front of the camera)
//   scene-select <name>|#<id>|none        select an instance
//   scene-select-next / scene-select-prev select in outliner order
//   scene-select-object sun|environment|player-start|light <index>
//   scene-move <x> <y> <z>                set the selected instance's position
//   scene-rotate <x> <y> <z>              set its rotation (Euler degrees)
//   scene-scale <s> | <x> <y> <z>         set its scale
//   scene-rename <name>                   rename it
//   scene-visible 0|1                     hide / show it
//   scene-parent <name>|#<id>|root        reparent it (keeps the world pose)
//   scene-duplicate / scene-delete / scene-group [name] / scene-ungroup
//   scene-override <material> replace <library.pbo> <material>
//   scene-override <material> <param> <values...>
//       params: base_color r g b a | metallic v | roughness v |
//               emissive r g b | emissive_strength v | uv_scale u v
//   scene-override <material> clear       remove the override
//   scene-physics off | static | dynamic | default | mass <v> |
//                 friction <v> | restitution <v> | clear
//   scene-sun <elevation_deg> <azimuth_deg> [intensity]
//   scene-sun-align                       align the sun to the HDRI
//   scene-env hdri <path>|default | intensity <v> | rotation <deg> |
//             exposure <ev>
//   scene-light-add [x y z]               add a point light (selected)
//   scene-light <index> position x y z | color r g b | intensity v | range v
//   scene-player-start <x> <y> <z> [yaw_deg [pitch_deg]]
//   scene-gravity <x> <y> <z>
//   scene-frame                           frame the selection in the viewport
//   scene-problems                        print the problems to the status bar

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "core/log.h"
#include "core/result.h"
#include "editor/commands.h"
#include "editor/editor_context.h"
#include "editor/scene_mode/scene_edit.h"
#include "editor/scene_mode/scene_mode.h"
#include "editor/selection.h"
#include "editor/viewport/viewport_panel.h"
#include "scene/scene.h"
#include "scene/scene_data.h"

namespace blue::editor {
namespace {

using Status = core::Result<void, std::string>;
using Args = std::span<const std::string>;

// Parses `count` floats starting at args[first].
core::Result<std::vector<float>, std::string> ParseFloats(
    Args args, size_t first, size_t count, std::string_view what) {
  using FloatsResult = core::Result<std::vector<float>, std::string>;
  if (args.size() < first + count) {
    return FloatsResult::Error(
        std::format("{} needs {} number(s)", what, count));
  }
  std::vector<float> values;
  for (size_t i = 0; i < count; ++i) {
    auto value = ParseFloatArg(args[first + i], what);
    if (!value.ok()) {
      return FloatsResult::Error(value.error());
    }
    values.push_back(value.value());
  }
  return FloatsResult::Ok(std::move(values));
}

// Resolves "<name>" or "#<id>" to an instance id.
core::Result<scene::InstanceId, std::string> ResolveInstance(
    const scene::SceneData& scene, const std::string& text) {
  using IdResult = core::Result<scene::InstanceId, std::string>;
  if (text.starts_with("#")) {
    auto id = ParseIntArg(std::string_view(text).substr(1), "instance id");
    if (!id.ok() || id.value() <= 0 ||
        scene::FindInstance(
            scene, static_cast<scene::InstanceId>(id.value())) == nullptr) {
      return IdResult::Error("no instance " + text);
    }
    return IdResult::Ok(static_cast<scene::InstanceId>(id.value()));
  }
  const scene::InstanceId id = FindInstanceByName(scene, text);
  if (id == scene::kNoInstance) {
    return IdResult::Error("no instance named '" + text + "'");
  }
  return IdResult::Ok(id);
}

// The document plus the primary selected instance, for commands acting on
// it.
struct Target {
  SceneDocument* doc = nullptr;
  scene::InstanceId id = scene::kNoInstance;
};

core::Result<Target, std::string> SelectedInstance(SceneModeState& state) {
  using TargetResult = core::Result<Target, std::string>;
  SceneDocument* doc = state.document();
  if (doc == nullptr) {
    return TargetResult::Error("no scene open");
  }
  const int64_t index = doc->selection().PrimaryIndex(SelectionKind::kInstance);
  if (index < 0) {
    return TargetResult::Error("no instance selected");
  }
  return TargetResult::Ok({doc, static_cast<scene::InstanceId>(index)});
}

Status RequireDocument(SceneModeState& state) {
  return state.document() == nullptr ? Status::Error("no scene open")
                                     : Status::Ok();
}

Status OverrideCommand(SceneModeState& state, Args args) {
  auto target = SelectedInstance(state);
  if (!target.ok()) {
    return Status::Error(target.error());
  }
  if (args.size() < 2) {
    return Status::Error("usage: scene-override <material> <param> ...");
  }
  SceneDocument& doc = *target.value().doc;
  const scene::InstanceId id = target.value().id;
  const std::string& material = args[0];
  const std::string& param = args[1];
  if (param == "clear") {
    bool removed = false;
    doc.Edit("Remove override", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               removed = RemoveMaterialOverride(scene, id, material);
             });
    return removed ? Status::Ok()
                   : Status::Error("no override for '" + material + "'");
  }
  if (param == "replace") {
    if (args.size() < 4) {
      return Status::Error(
          "usage: scene-override <material> replace <library.pbo> <name>");
    }
    bool done = false;
    doc.Edit("Replace material", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               done = SetMaterialReplacement(scene, id, material, args[2],
                                             args[3]);
             });
    return done ? Status::Ok() : Status::Error("cannot set the replacement");
  }
  const auto apply = [&](auto&& set) {
    doc.Edit("Material tweak", ChangeKind::kStructure,
             [&](scene::SceneData& scene) {
               set(FindOrAddMaterialOverride(*scene::FindInstance(scene, id),
                                             material));
             });
    return Status::Ok();
  };
  const auto floats = [&](size_t count) {
    return ParseFloats(args, 2, count, param);
  };
  if (param == "base_color") {
    auto v = floats(4);
    if (!v.ok()) {
      return Status::Error(v.error());
    }
    const std::vector<float> c = v.value();
    return apply([&](scene::MaterialOverride& o) {
      o.base_color = glm::vec4(c[0], c[1], c[2], c[3]);
    });
  }
  if (param == "emissive") {
    auto v = floats(3);
    if (!v.ok()) {
      return Status::Error(v.error());
    }
    const std::vector<float> c = v.value();
    return apply([&](scene::MaterialOverride& o) {
      o.emissive = glm::vec3(c[0], c[1], c[2]);
    });
  }
  if (param == "uv_scale") {
    auto v = floats(2);
    if (!v.ok()) {
      return Status::Error(v.error());
    }
    const std::vector<float> c = v.value();
    return apply([&](scene::MaterialOverride& o) {
      o.uv_scale = glm::vec2(c[0], c[1]);
    });
  }
  std::optional<float> scene::MaterialOverride::* member = nullptr;
  if (param == "metallic") {
    member = &scene::MaterialOverride::metallic;
  } else if (param == "roughness") {
    member = &scene::MaterialOverride::roughness;
  } else if (param == "emissive_strength") {
    member = &scene::MaterialOverride::emissive_strength;
  } else {
    return Status::Error("unknown override parameter '" + param + "'");
  }
  auto v = floats(1);
  if (!v.ok()) {
    return Status::Error(v.error());
  }
  const float value = v.value()[0];
  return apply([&](scene::MaterialOverride& o) { o.*member = value; });
}

Status PhysicsCommand(SceneModeState& state, Args args) {
  auto target = SelectedInstance(state);
  if (!target.ok()) {
    return Status::Error(target.error());
  }
  if (args.empty()) {
    return Status::Error(
        "usage: scene-physics off|static|dynamic|default|"
        "mass v|friction v|restitution v|clear");
  }
  SceneDocument& doc = *target.value().doc;
  const scene::InstanceId id = target.value().id;
  const std::string& what = args[0];
  std::optional<float> value;
  if (what == "mass" || what == "friction" || what == "restitution") {
    auto parsed = ParseFloats(args, 1, 1, what);
    if (!parsed.ok()) {
      return Status::Error(parsed.error());
    }
    value = parsed.value()[0];
  } else if (what != "off" && what != "static" && what != "dynamic" &&
             what != "default" && what != "clear") {
    return Status::Error("unknown physics setting '" + what + "'");
  }
  doc.Edit(
      "Physics override", ChangeKind::kStructure, [&](scene::SceneData& scene) {
        scene::Instance* instance = scene::FindInstance(scene, id);
        if (what == "clear") {
          instance->physics.reset();
          return;
        }
        scene::PhysicsOverride& physics = instance->physics.has_value()
                                              ? *instance->physics
                                              : instance->physics.emplace();
        if (what == "off") {
          physics.disabled = true;
        } else if (what == "static") {
          physics.disabled = false;
          physics.motion = scene::MotionType::kStatic;
        } else if (what == "dynamic") {
          physics.disabled = false;
          physics.motion = scene::MotionType::kDynamic;
        } else if (what == "default") {
          physics.disabled = false;
          physics.motion.reset();
        } else if (what == "mass") {
          physics.mass = value;
        } else if (what == "friction") {
          physics.friction = value;
        } else {
          physics.restitution = value;
        }
      });
  // Values out of range are left to the save check, so report them now.
  if (auto error = scene::ValidateSceneData(doc.state())) {
    return Status::Error(*error);
  }
  return Status::Ok();
}

Status EnvironmentCommand(SceneModeState& state, Args args) {
  if (auto ok = RequireDocument(state); !ok.ok()) {
    return ok;
  }
  if (args.size() < 2) {
    return Status::Error(
        "usage: scene-env hdri <path>|default | intensity v | "
        "rotation deg | exposure ev");
  }
  SceneDocument& doc = *state.document();
  const std::string& what = args[0];
  if (what == "hdri") {
    const std::string path = args[1] == "default" ? std::string() : args[1];
    doc.Edit("Set HDRI", ChangeKind::kParameters, [&](scene::SceneData& scene) {
      scene.environment.hdri_path = path;
    });
    return Status::Ok();
  }
  auto parsed = ParseFloats(args, 1, 1, what);
  if (!parsed.ok()) {
    return Status::Error(parsed.error());
  }
  const float value = parsed.value()[0];
  if (what != "intensity" && what != "rotation" && what != "exposure") {
    return Status::Error("unknown environment setting '" + what + "'");
  }
  doc.Edit("Environment", ChangeKind::kParameters,
           [&](scene::SceneData& scene) {
             if (what == "intensity") {
               scene.environment.intensity = std::max(value, 0.0f);
             } else if (what == "rotation") {
               scene.environment.rotation_radians = glm::radians(value);
             } else {
               scene.environment.exposure_ev = value;
             }
           });
  return Status::Ok();
}

Status LightCommand(SceneModeState& state, Args args) {
  if (auto ok = RequireDocument(state); !ok.ok()) {
    return ok;
  }
  SceneDocument& doc = *state.document();
  if (args.size() < 3) {
    return Status::Error(
        "usage: scene-light <index> position x y z | "
        "color r g b | intensity v | range v");
  }
  auto index = ParseIntArg(args[0], "light index");
  if (!index.ok()) {
    return Status::Error(index.error());
  }
  if (index.value() < 0 ||
      static_cast<size_t>(index.value()) >= doc.state().point_lights.size()) {
    return Status::Error("no light " + args[0]);
  }
  const auto light = static_cast<size_t>(index.value());
  const std::string& what = args[1];
  const size_t count = what == "position" || what == "color" ? 3 : 1;
  auto parsed = ParseFloats(args, 2, count, what);
  if (!parsed.ok()) {
    return Status::Error(parsed.error());
  }
  const std::vector<float> v = parsed.value();
  if (what != "position" && what != "color" && what != "intensity" &&
      what != "range") {
    return Status::Error("unknown light setting '" + what + "'");
  }
  doc.Edit("Edit light", ChangeKind::kParameters, [&](scene::SceneData& scene) {
    scene::ScenePointLight& edited = scene.point_lights[light];
    if (what == "position") {
      edited.position = {v[0], v[1], v[2]};
    } else if (what == "color") {
      edited.color = glm::max(glm::vec3(v[0], v[1], v[2]), 0.0f);
    } else if (what == "intensity") {
      edited.intensity = std::max(v[0], 0.0f);
    } else {
      edited.range = std::max(v[0], 0.01f);
    }
  });
  doc.selection().Set(
      {SelectionKind::kScenePointLight, static_cast<int64_t>(light)});
  return Status::Ok();
}

// Registers the commands that act on the selected instance's transform and
// identity.
void RegisterInstanceCommands(SceneModeState& state,
                              CommandRegistry& commands) {
  SceneModeState* s = &state;
  commands.Register(
      "scene-add", "scene-add <file.pbo> [x y z]: places an instance",
      [s](Args args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        if (args.empty()) {
          return Status::Error("usage: scene-add <file.pbo>");
        }
        glm::vec3 position = s->DropPoint(std::nullopt);
        if (args.size() >= 4) {
          auto v = ParseFloats(args, 1, 3, "position");
          if (!v.ok()) {
            return Status::Error(v.error());
          }
          position = {v.value()[0], v.value()[1], v.value()[2]};
        }
        return s->PlaceInstance(args[0], position) != scene::kNoInstance
                   ? Status::Ok()
                   : Status::Error("cannot place " + args[0]);
      });
  commands.Register("scene-select",
                    "scene-select <name>|#<id>|none: selects an instance",
                    [s](Args args) {
                      if (auto ok = RequireDocument(*s); !ok.ok()) {
                        return ok;
                      }
                      if (args.empty())
                        return Status::Error("usage: scene-select <name>");
                      SceneDocument& doc = *s->document();
                      if (args[0] == "none") {
                        doc.selection().Clear();
                        return Status::Ok();
                      }
                      auto id = ResolveInstance(doc.state(), args[0]);
                      if (!id.ok()) {
                        return Status::Error(id.error());
                      }
                      doc.selection().Set(InstanceItem(id.value()));
                      return Status::Ok();
                    });
  commands.Register("scene-select-next",
                    "scene-select-next: selects the next instance", [s](Args) {
                      return s->StepSelection(1)
                                 ? Status::Ok()
                                 : Status::Error("no instances");
                    });
  commands.Register(
      "scene-select-prev", "scene-select-prev: selects the previous instance",
      [s](Args) {
        return s->StepSelection(-1) ? Status::Ok()
                                    : Status::Error("no instances");
      });
  commands.Register(
      "scene-select-object",
      "scene-select-object sun|environment|player-start|light <i>",
      [s](Args args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        if (args.empty()) {
          return Status::Error("what to select?");
        }
        Selection& selection = s->document()->selection();
        if (args[0] == "sun") {
          selection.Set({SelectionKind::kSun, -1});
        } else if (args[0] == "environment") {
          selection.Set({SelectionKind::kEnvironment, -1});
        } else if (args[0] == "player-start") {
          selection.Set({SelectionKind::kPlayerStart, -1});
        } else if (args[0] == "light" && args.size() >= 2) {
          auto index = ParseIntArg(args[1], "light index");
          if (!index.ok()) {
            return Status::Error(index.error());
          }
          if (index.value() < 0 ||
              static_cast<size_t>(index.value()) >=
                  s->document()->state().point_lights.size()) {
            return Status::Error("no light " + args[1]);
          }
          selection.Set({SelectionKind::kScenePointLight, index.value()});
        } else {
          return Status::Error("unknown object '" + args[0] + "'");
        }
        return Status::Ok();
      });
  const auto transform_command = [s](std::string_view label, auto&& apply) {
    return [s, label, apply](Args args) {
      auto target = SelectedInstance(*s);
      if (!target.ok()) {
        return Status::Error(target.error());
      }
      const size_t count = args.size() == 1 ? 1 : 3;
      auto v = ParseFloats(args, 0, count, label);
      if (!v.ok()) {
        return Status::Error(v.error());
      }
      const std::vector<float> values = v.value();
      const scene::InstanceId id = target.value().id;
      target.value().doc->Edit(
          label, ChangeKind::kParameters, [&](scene::SceneData& scene) {
            apply(scene::FindInstance(scene, id)->transform, values);
          });
      return Status::Ok();
    };
  };
  commands.Register(
      "scene-move", "scene-move <x> <y> <z>: sets the position",
      transform_command("Move instance",
                        [](scene::Transform& t, const std::vector<float>& v) {
                          if (v.size() == 3) {
                            t.position = {v[0], v[1], v[2]};
                          }
                        }));
  commands.Register(
      "scene-rotate", "scene-rotate <x> <y> <z>: sets the rotation (degrees)",
      transform_command("Rotate instance", [](scene::Transform& t,
                                              const std::vector<float>& v) {
        if (v.size() == 3) {
          t.rotation = glm::quat(glm::radians(glm::vec3(v[0], v[1], v[2])));
        }
      }));
  commands.Register(
      "scene-scale", "scene-scale <s> | <x> <y> <z>: sets the scale",
      transform_command("Scale instance", [](scene::Transform& t,
                                             const std::vector<float>& v) {
        t.scale = v.size() == 1 ? glm::vec3(v[0]) : glm::vec3(v[0], v[1], v[2]);
      }));
  commands.Register(
      "scene-rename", "scene-rename <name>: renames the selected instance",
      [s](Args args) {
        auto target = SelectedInstance(*s);
        if (!target.ok()) {
          return Status::Error(target.error());
        }
        if (args.empty()) {
          return Status::Error("usage: scene-rename <name>");
        }
        const scene::InstanceId id = target.value().id;
        target.value().doc->Edit(
            "Rename", ChangeKind::kParameters, [&](scene::SceneData& scene) {
              scene::FindInstance(scene, id)->name = args[0];
            });
        return Status::Ok();
      });
  commands.Register("scene-visible",
                    "scene-visible 0|1: hides or shows the instance",
                    [s](Args args) {
                      auto target = SelectedInstance(*s);
                      if (!target.ok()) {
                        return Status::Error(target.error());
                      }
                      if (args.empty())
                        return Status::Error("usage: scene-visible 0|1");
                      const bool visible = args[0] != "0";
                      const scene::InstanceId id = target.value().id;
                      target.value().doc->Edit(
                          visible ? "Show" : "Hide", ChangeKind::kStructure,
                          [&](scene::SceneData& scene) {
                            scene::FindInstance(scene, id)->visible = visible;
                          });
                      return Status::Ok();
                    });
  commands.Register(
      "scene-parent", "scene-parent <name>|#<id>|root: reparents the instance",
      [s](Args args) {
        auto target = SelectedInstance(*s);
        if (!target.ok()) {
          return Status::Error(target.error());
        }
        if (args.empty()) {
          return Status::Error("usage: scene-parent <name>");
        }
        SceneDocument& doc = *target.value().doc;
        scene::InstanceId parent = scene::kNoInstance;
        if (args[0] != "root") {
          auto resolved = ResolveInstance(doc.state(), args[0]);
          if (!resolved.ok()) {
            return Status::Error(resolved.error());
          }
          parent = resolved.value();
        }
        bool moved = false;
        const scene::InstanceId id = target.value().id;
        doc.Edit("Reparent", ChangeKind::kStructure,
                 [&](scene::SceneData& scene) {
                   moved = scene::ReparentInstance(scene, id, parent, true);
                 });
        return moved ? Status::Ok()
                     : Status::Error(
                           "cannot move an instance into itself or "
                           "its children");
      });
}

}  // namespace

void RegisterSceneCommands(SceneModeState& state) {
  CommandRegistry& commands = state.context().commands();
  SceneModeState* s = &state;
  RegisterInstanceCommands(state, commands);
  const auto selection_command = [s](bool (SceneModeState::*action)(),
                                     const char* error) {
    return [s, action, error](Args) {
      return (s->*action)() ? Status::Ok() : Status::Error(error);
    };
  };
  commands.Register("scene-duplicate",
                    "scene-duplicate: duplicates the selection (Ctrl+D)",
                    selection_command(&SceneModeState::DuplicateSelection,
                                      "nothing selected"));
  commands.Register(
      "scene-delete", "scene-delete: deletes the selection (Del)",
      selection_command(&SceneModeState::DeleteSelection, "nothing selected"));
  commands.Register("scene-group",
                    "scene-group: groups the selected instances (Ctrl+G)",
                    selection_command(&SceneModeState::GroupSelection,
                                      "no instance selected"));
  commands.Register(
      "scene-ungroup",
      "scene-ungroup: ungroups the selected instance (Ctrl+Shift+G)",
      selection_command(&SceneModeState::UngroupSelection,
                        "no group selected"));
  commands.Register(
      "scene-override",
      "scene-override <material> replace <lib.pbo> <name> | <param> <values> "
      "| clear: material override of the selected instance",
      [s](Args args) { return OverrideCommand(*s, args); });
  commands.Register(
      "scene-physics",
      "scene-physics off|static|dynamic|default|mass v|friction v|"
      "restitution v|clear: physics override of the selected instance",
      [s](Args args) { return PhysicsCommand(*s, args); });
  commands.Register(
      "scene-sun", "scene-sun <elevation_deg> <azimuth_deg> [intensity]",
      [s](Args args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        auto v = ParseFloats(args, 0, args.size() >= 3 ? 3 : 2, "sun");
        if (!v.ok()) {
          return Status::Error(v.error());
        }
        const std::vector<float> values = v.value();
        s->Edit("Sun", ChangeKind::kParameters, [&](scene::SceneData& scene) {
          scene.sun.direction = scene::SunDirectionFromAngles(
              {glm::radians(values[0]), glm::radians(values[1])});
          if (values.size() == 3) {
            scene.sun.intensity = std::max(values[2], 0.0f);
          }
        });
        return Status::Ok();
      });
  commands.Register("scene-sun-align",
                    "scene-sun-align: aligns the sun to the HDRI",
                    [s](Args) { return s->AlignSunToHdri(); });
  commands.Register(
      "scene-env",
      "scene-env hdri <path>|default | intensity v | rotation deg | "
      "exposure ev",
      [s](Args args) { return EnvironmentCommand(*s, args); });
  commands.Register("scene-light-add",
                    "scene-light-add [x y z]: adds a point light",
                    [s](Args args) {
                      if (auto ok = RequireDocument(*s); !ok.ok()) {
                        return ok;
                      }
                      glm::vec3 position = s->DropPoint(std::nullopt) +
                                           glm::vec3(0.0f, 2.0f, 0.0f);
                      if (args.size() >= 3) {
                        auto v = ParseFloats(args, 0, 3, "position");
                        if (!v.ok()) {
                          return Status::Error(v.error());
                        }
                        position = {v.value()[0], v.value()[1], v.value()[2]};
                      }
                      s->AddPointLight(position);
                      return Status::Ok();
                    });
  commands.Register(
      "scene-light",
      "scene-light <index> position x y z | color r g b | intensity v | "
      "range v",
      [s](Args args) { return LightCommand(*s, args); });
  commands.Register(
      "scene-player-start",
      "scene-player-start <x> <y> <z> [yaw_deg [pitch_deg]]", [s](Args args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        const size_t count = std::min<size_t>(args.size(), 5);
        if (count < 3) {
          return Status::Error("usage: scene-player-start <x> <y> <z> [yaw]");
        }
        auto v = ParseFloats(args, 0, count, "player start");
        if (!v.ok()) {
          return Status::Error(v.error());
        }
        const std::vector<float> values = v.value();
        s->Edit("Player start", ChangeKind::kParameters,
                [&](scene::SceneData& scene) {
                  scene.player_start.position = {values[0], values[1],
                                                 values[2]};
                  if (values.size() >= 4) {
                    scene.player_start.yaw_radians = glm::radians(values[3]);
                  }
                  if (values.size() >= 5) {
                    scene.player_start.pitch_radians = glm::radians(values[4]);
                  }
                });
        return Status::Ok();
      });
  commands.Register(
      "scene-gravity", "scene-gravity <x> <y> <z>: sets the gravity",
      [s](Args args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        auto v = ParseFloats(args, 0, 3, "gravity");
        if (!v.ok()) {
          return Status::Error(v.error());
        }
        const std::vector<float> values = v.value();
        s->Edit("Gravity", ChangeKind::kParameters,
                [&](scene::SceneData& scene) {
                  scene.gravity = {values[0], values[1], values[2]};
                });
        return Status::Ok();
      });
  commands.Register(
      "scene-frame", "scene-frame: frames the selection in the viewport",
      [s](Args) {
        ViewportPanel& viewport = s->context().viewport();
        // The scene client is the one registered for Scene mode; go through
        // the bounds the outliner uses.
        if (s->world() == nullptr || s->document() == nullptr) {
          return Status::Error("no scene open");
        }
        std::optional<asset::Bounds> total;
        const auto ids = s->document()->selection().Instances();
        const auto& instances = s->document()->state().instances;
        std::vector<scene::InstanceId> all;
        for (const scene::Instance& instance : instances) {
          all.push_back(instance.id);
        }
        for (const scene::InstanceId id : ids.empty() ? all : ids) {
          if (auto bounds = s->world()->InstanceBounds(id)) {
            if (!total) {
              total = *bounds;
            } else {
              total->min = glm::min(total->min, bounds->min);
              total->max = glm::max(total->max, bounds->max);
            }
          }
        }
        if (!total) {
          return Status::Error("nothing with bounds to frame");
        }
        viewport.FrameBounds(*total);
        return Status::Ok();
      });
  commands.Register(
      "scene-problems",
      "scene-problems: shows the problem count and the "
      "first problem in the status bar (all of them in the log)",
      [s](Args) {
        if (auto ok = RequireDocument(*s); !ok.ok()) {
          return ok;
        }
        const auto problems = s->problems();
        for (const SceneModeState::Problem& problem : problems) {
          core::log::Info("Scene problem (instance {}): {}", problem.instance,
                          problem.message);
        }
        s->context().Status(problems.empty()
                                ? std::string("Scene: no problems")
                                : std::format("Scene: {} problem(s); first: {}",
                                              problems.size(),
                                              problems.front().message));
        return Status::Ok();
      });
}

}  // namespace blue::editor
