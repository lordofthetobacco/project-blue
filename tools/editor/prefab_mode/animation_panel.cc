#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "animation/clip.h"
#include "cook/clip_tools.h"
#include "cook/cook_error.h"
#include "core/result.h"
#include "editor/document.h"
#include "editor/editor_context.h"
#include "editor/prefab_mode/prefab_mode_state.h"
#include "editor/prefab_mode/prefab_panels.h"
#include "editor/prefab_mode/prefab_widgets.h"
#include "editor/selection.h"
#include "editor/undo.h"
#include "prefab/prefab.h"
#include "world/prefab_pose.h"

namespace blue::editor {

namespace {

constexpr float kMarkerHalfWidth = 5.0f;  // px, event marker hit area
constexpr ImU32 kPlayheadColor = IM_COL32(255, 80, 60, 255);
constexpr ImU32 kEventColor = IM_COL32(120, 200, 255, 255);
constexpr ImU32 kTrimColor = IM_COL32(255, 200, 60, 40);

// Returns the time step between labelled ticks for `duration` seconds over
// `width` pixels (about one label per 70 px).
[[nodiscard]] float TickStep(float duration, float width) {
  const float wanted = duration * 70.0f / std::max(width, 1.0f);
  for (const float step : {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.25f, 0.5f, 1.0f,
                           2.0f, 5.0f, 10.0f, 30.0f, 60.0f}) {
    if (step >= wanted) {
      return step;
    }
  }
  return 120.0f;
}

// Shows the clip of `preview` at its time on `pose` without playing.
void ShowFrame(world::PrefabPose& pose, const AnimationPreview& preview) {
  if (pose.clip() != preview.clip) {
    pose.Play(preview.clip, preview.time, 0.0f, preview.loop);
    pose.Stop();
  }
  pose.SetTime(preview.time);
}

// Runs a clip_tools edit as one undo step (structural: the preview's clips
// live in the GPU copy).
template <typename Op>
bool ClipEdit(EditorContext& context, PrefabDocument& document,
              std::string_view label, Op&& op) {
  return TryEdit(context, document, label, ChangeKind::kStructure,
                 [&](prefab::Prefab& prefab) -> std::optional<std::string> {
                   const auto result = op(prefab);
                   if (!result.ok()) {
                     return std::string(label) +
                            " failed: " + CookErrorText(result.error());
                   }
                   return std::nullopt;
                 });
}

}  // namespace

void PrefabAnimationPanel::Update(EditorContext& context, float dt) {
  AnimationPreview& preview = shared_.preview;
  PrefabDocument* document = context.prefab_document();
  world::PrefabPose* pose = context.prefab_pose();
  if (document == nullptr || pose == nullptr) {
    preview.playing = false;
    return;
  }
  const int clip_count = static_cast<int>(document->state().clips.size());
  if (preview.clip >= clip_count) {
    preview.clip = clip_count - 1;
    preview.playing = false;
  }
  if (preview.clip < 0) {
    if (pose->clip() >= 0) {
      pose->ResetToRest();
    }
    return;
  }
  // The shell recreates the pose after structural edits: carry on.
  if (pose->clip() != preview.clip) {
    ShowFrame(*pose, preview);
    if (preview.playing) {
      pose->Play(preview.clip, preview.time, preview.speed, preview.loop);
    }
  }
  if (preview.playing) {
    pose->Update(dt);
    preview.time = pose->player().time();
    if (!pose->player().playing()) {
      preview.playing = false;
    }
  }
}

void PrefabAnimationPanel::Draw(EditorContext& context) {
  PrefabDocument* document = context.prefab_document();
  if (document == nullptr) {
    ImGui::TextDisabled("No prefab open.");
    return;
  }
  if (document->state().clips.empty()) {
    ImGui::TextDisabled("This prefab has no animation clips.");
    return;
  }
  const int selected = static_cast<int>(
      document->selection().PrimaryIndex(SelectionKind::kClip));
  if (selected >= 0) {
    shared_.preview.clip = selected;
  }
  if (shared_.preview.clip < 0) {
    shared_.preview.clip = 0;
  }
  if (ImGui::BeginTable(
          "##anim", 2,
          ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
    ImGui::TableSetupColumn("clips", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 18.0f);
    ImGui::TableNextColumn();
    DrawClipList(context, *document);
    ImGui::TableNextColumn();
    if (shared_.preview.clip <
        static_cast<int>(document->state().clips.size())) {
      DrawTimeline(context, *document);
      DrawEvents(context, *document);
    }
    ImGui::EndTable();
  }
}

void PrefabAnimationPanel::DrawClipList(EditorContext& context,
                                        PrefabDocument& document) {
  const prefab::Prefab& prefab = document.state();
  merge_checked_.resize(prefab.clips.size(), false);
  int checked = 0;
  for (size_t i = 0; i < prefab.clips.size(); ++i) {
    const animation::Clip& clip = prefab.clips[i];
    ImGui::PushID(static_cast<int>(i));
    bool check = merge_checked_[i];
    if (ImGui::Checkbox("##merge", &check)) {
      merge_checked_[i] = check;
    }
    checked += merge_checked_[i] ? 1 : 0;
    ImGui::SameLine();
    const std::string label = std::format(
        "{} ({:.2f} s{})", clip.name, clip.duration, clip.loop ? ", loop" : "");
    if (ImGui::Selectable(label.c_str(),
                          shared_.preview.clip == static_cast<int>(i))) {
      shared_.preview.clip = static_cast<int>(i);
      shared_.preview.time = 0.0f;
      shared_.preview.playing = false;
      shared_.preview.loop = clip.loop;
      document.selection().Set(
          {.kind = SelectionKind::kClip, .index = static_cast<int64_t>(i)});
    }
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
  InputString("##merge_name", merge_name_);
  ImGui::SameLine();
  ImGui::BeginDisabled(checked < 2 || merge_name_.empty());
  if (ImGui::Button("Merge checked")) {
    std::vector<int> clips;
    for (size_t i = 0; i < merge_checked_.size(); ++i) {
      if (merge_checked_[i]) {
        clips.push_back(static_cast<int>(i));
      }
    }
    int merged = -1;
    const bool done =
        ClipEdit(context, document, "Merge clips", [&](prefab::Prefab& p) {
          core::Result<int, cook::CookError> result =
              cook::MergeClips(p, clips, merge_name_);
          if (result.ok()) {
            merged = result.value();
          }
          return result;
        });
    if (done) {
      merge_checked_.assign(document.state().clips.size(), false);
      shared_.preview = {.clip = merged, .loop = false};
      document.selection().Set({.kind = SelectionKind::kClip, .index = merged});
    }
  }
  ImGui::EndDisabled();
}

void PrefabAnimationPanel::DrawTimeline(EditorContext& context,
                                        PrefabDocument& document) {
  AnimationPreview& preview = shared_.preview;
  const int clip_index = preview.clip;
  const animation::Clip& clip =
      document.state().clips[static_cast<size_t>(clip_index)];
  const float duration = std::max(clip.duration, 1e-3f);
  world::PrefabPose* pose = context.prefab_pose();

  // Name, authored loop flag.
  std::string name;
  if (InputTextCommit(shared_, "Clip name", clip.name, name)) {
    ClipEdit(context, document, "Rename clip", [&](prefab::Prefab& p) {
      return cook::RenameClip(p, clip_index, std::move(name));
    });
  }
  bool loop = clip.loop;
  if (ImGui::Checkbox("Loops (authored)", &loop)) {
    ClipEdit(context, document, "Clip loop", [&](prefab::Prefab& p) {
      return cook::SetClipLoop(p, clip_index, loop);
    });
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete clip")) {
    if (TryEdit(context, document, "Delete clip", ChangeKind::kStructure,
                [&](prefab::Prefab& p) {
                  return DeleteItem(
                      p, {.kind = SelectionKind::kClip, .index = clip_index});
                })) {
      preview = {};
      document.selection().Clear();
      return;
    }
  }

  // Transport.
  if (ImGui::Button(preview.playing ? "Pause" : "Play") && pose != nullptr) {
    if (preview.playing) {
      pose->Stop();
      preview.playing = false;
    } else {
      if (!preview.loop && preview.time >= clip.duration) {
        preview.time = 0.0f;
      }
      pose->Play(clip_index, preview.time, preview.speed, preview.loop);
      preview.playing = true;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Stop") && pose != nullptr) {
    preview.playing = false;
    preview.time = 0.0f;
    ShowFrame(*pose, preview);
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  if (ImGui::SliderFloat("Speed", &preview.speed, 0.05f, 2.0f, "%.2fx") &&
      preview.playing && pose != nullptr) {
    pose->Play(clip_index, preview.time, preview.speed, preview.loop);
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Loop preview", &preview.loop) && preview.playing &&
      pose != nullptr) {
    pose->Play(clip_index, preview.time, preview.speed, preview.loop);
  }
  ImGui::SameLine();
  ImGui::Text("%.3f / %.3f s", static_cast<double>(preview.time),
              static_cast<double>(clip.duration));

  // The timeline canvas.
  const float width = std::max(ImGui::GetContentRegionAvail().x, 50.0f);
  const float height = ImGui::GetFrameHeight() * 2.6f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##timeline", {width, height});
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  ImDrawList* draw = ImGui::GetWindowDrawList();
  const auto x_of = [&](float t) { return origin.x + t / duration * width; };
  const auto t_of = [&](float x) {
    return std::clamp((x - origin.x) / width * duration, 0.0f, clip.duration);
  };
  draw->AddRectFilled(origin, {origin.x + width, origin.y + height},
                      ImGui::GetColorU32(ImGuiCol_FrameBg));
  if (trim_clip_ != clip_index) {
    trim_clip_ = clip_index;
    trim_start_ = 0.0f;
    trim_end_ = clip.duration;
  }
  draw->AddRectFilled({x_of(trim_start_), origin.y},
                      {x_of(trim_end_), origin.y + height}, kTrimColor);
  const float step = TickStep(duration, width);
  for (float t = 0.0f; t <= duration + 1e-4f; t += step) {
    const float x = x_of(t);
    draw->AddLine({x, origin.y + height * 0.7f}, {x, origin.y + height},
                  ImGui::GetColorU32(ImGuiCol_TextDisabled));
    const std::string tick =
        std::format("{:g}", std::round(t * 1000.0f) / 1000.0f);
    draw->AddText({x + 2.0f, origin.y + height * 0.62f - ImGui::GetFontSize()},
                  ImGui::GetColorU32(ImGuiCol_TextDisabled), tick.c_str());
  }

  // Events: markers at the top, draggable.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  int hovered_event = -1;
  for (size_t e = 0; e < clip.events.size(); ++e) {
    const bool dragging = dragged_event_ == static_cast<int>(e);
    const float t = dragging ? dragged_time_ : clip.events[e].time;
    const float x = x_of(t);
    draw->AddTriangleFilled(
        {x - kMarkerHalfWidth, origin.y}, {x + kMarkerHalfWidth, origin.y},
        {x, origin.y + kMarkerHalfWidth * 1.6f}, kEventColor);
    draw->AddText({x + kMarkerHalfWidth, origin.y}, kEventColor,
                  clip.events[e].name.c_str());
    if (hovered && std::abs(mouse.x - x) <= kMarkerHalfWidth &&
        mouse.y <= origin.y + ImGui::GetFontSize()) {
      hovered_event = static_cast<int>(e);
    }
  }
  if (ImGui::IsItemActivated()) {
    if (hovered_event >= 0) {
      dragged_event_ = hovered_event;
      dragged_time_ = clip.events[static_cast<size_t>(hovered_event)].time;
    } else {
      dragged_event_ = -1;
    }
  }
  if (active) {
    if (dragged_event_ >= 0) {
      dragged_time_ = t_of(mouse.x);
    } else if (pose != nullptr) {
      preview.time = t_of(mouse.x);
      if (preview.playing) {
        pose->Stop();
        preview.playing = false;
      }
      ShowFrame(*pose, preview);
    }
  }
  if (ImGui::IsItemDeactivated() && dragged_event_ >= 0) {
    const int event = dragged_event_;
    const float time = dragged_time_;
    dragged_event_ = -1;
    const std::string event_name = clip.events[static_cast<size_t>(event)].name;
    ClipEdit(context, document, "Move event", [&](prefab::Prefab& p) {
      core::Result<void, cook::CookError> removed =
          cook::RemoveClipEvent(p, clip_index, event);
      if (!removed.ok()) {
        return removed;
      }
      core::Result<int, cook::CookError> added =
          cook::AddClipEvent(p, clip_index, event_name, time);
      return added.ok()
                 ? core::Result<void, cook::CookError>::Ok()
                 : core::Result<void, cook::CookError>::Error(added.error());
    });
  }
  const float playhead = x_of(preview.time);
  draw->AddLine({playhead, origin.y}, {playhead, origin.y + height},
                kPlayheadColor, 2.0f);
  if (hovered && hovered_event >= 0) {
    ImGui::SetTooltip(
        "%s at %.3f s (drag to move)",
        clip.events[static_cast<size_t>(hovered_event)].name.c_str(),
        static_cast<double>(
            clip.events[static_cast<size_t>(hovered_event)].time));
  }

  // Trim.
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  ImGui::DragFloatRange2("Trim range", &trim_start_, &trim_end_, 0.005f, 0.0f,
                         clip.duration, "%.3f s");
  ImGui::SameLine();
  if (ImGui::SmallButton("Start at playhead")) {
    trim_start_ = std::min(preview.time, trim_end_);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("End at playhead")) {
    trim_end_ = std::max(preview.time, trim_start_);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!(trim_start_ < trim_end_) ||
                       (trim_start_ <= 0.0f && trim_end_ >= clip.duration));
  if (ImGui::Button("Trim clip")) {
    const float start = trim_start_;
    const float end = trim_end_;
    if (ClipEdit(context, document, "Trim clip", [&](prefab::Prefab& p) {
          return cook::TrimClip(p, clip_index, start, end);
        })) {
      trim_clip_ = -1;
      preview.time = 0.0f;
    }
  }
  ImGui::EndDisabled();
}

void PrefabAnimationPanel::DrawEvents(EditorContext& context,
                                      PrefabDocument& document) {
  const int clip_index = shared_.preview.clip;
  ImGui::SeparatorText("Events");
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
  InputString("##new_event", new_event_name_);
  ImGui::SameLine();
  ImGui::BeginDisabled(new_event_name_.empty());
  if (ImGui::Button("Add at playhead")) {
    const float time = shared_.preview.time;
    ClipEdit(context, document, "Add event", [&](prefab::Prefab& p) {
      core::Result<int, cook::CookError> added =
          cook::AddClipEvent(p, clip_index, new_event_name_, time);
      return added.ok()
                 ? core::Result<void, cook::CookError>::Ok()
                 : core::Result<void, cook::CookError>::Error(added.error());
    });
  }
  ImGui::EndDisabled();

  const animation::Clip& clip =
      document.state().clips[static_cast<size_t>(clip_index)];
  std::optional<int> remove;
  for (size_t e = 0; e < clip.events.size(); ++e) {
    ImGui::PushID(static_cast<int>(e));
    std::string name;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (InputTextCommit(shared_, "##name", clip.events[e].name, name) &&
        !name.empty()) {
      document.Edit("Rename event", ChangeKind::kStructure,
                    [&](prefab::Prefab& p) {
                      p.clips[static_cast<size_t>(clip_index)].events[e].name =
                          std::move(name);
                    });
    }
    ImGui::SameLine();
    const bool dragging = dragged_event_ == static_cast<int>(e);
    float time = dragging ? dragged_time_ : clip.events[e].time;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    if (ImGui::DragFloat("s", &time, 0.002f, 0.0f, clip.duration, "%.3f")) {
      dragged_event_ = static_cast<int>(e);
      dragged_time_ = std::clamp(time, 0.0f, clip.duration);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && dragging) {
      const float moved_time = dragged_time_;
      const std::string event_name = clip.events[e].name;
      dragged_event_ = -1;
      ClipEdit(context, document, "Move event", [&](prefab::Prefab& p) {
        core::Result<void, cook::CookError> removed =
            cook::RemoveClipEvent(p, clip_index, static_cast<int>(e));
        if (!removed.ok()) {
          return removed;
        }
        core::Result<int, cook::CookError> added =
            cook::AddClipEvent(p, clip_index, event_name, moved_time);
        return added.ok()
                   ? core::Result<void, cook::CookError>::Ok()
                   : core::Result<void, cook::CookError>::Error(added.error());
      });
      ImGui::PopID();
      break;  // `clip` changed.
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Go")) {
      shared_.preview.time = clip.events[e].time;
      shared_.preview.playing = false;
      if (world::PrefabPose* pose = context.prefab_pose()) {
        pose->Stop();
        ShowFrame(*pose, shared_.preview);
      }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      remove = static_cast<int>(e);
    }
    ImGui::PopID();
  }
  if (remove) {
    ClipEdit(context, document, "Delete event", [&](prefab::Prefab& p) {
      return cook::RemoveClipEvent(p, clip_index, *remove);
    });
  }
}

}  // namespace blue::editor
