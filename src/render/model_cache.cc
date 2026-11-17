#include "render/model_cache.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "asset/animated_model.h"
#include "asset/model_importer.h"
#include "core/log.h"
#include "platform/file_system.h"

namespace blue::render {

const UploadedModel* ModelCache::Get(std::string_view path) {
  std::string key = platform::AbsolutePath(path);
  if (const auto found = models_.find(key); found != models_.end()) {
    return found->second.get();
  }

  const auto start = std::chrono::steady_clock::now();
  auto imported = asset::ImportAnimatedModel(key);
  if (!imported.ok()) {
    core::log::Error("Failed to import '{}': {}", path,
                     asset::ToString(imported.error()));
    models_.emplace(std::move(key), nullptr);
    return nullptr;
  }
  asset::AnimatedModel model = std::move(imported).value();

  auto uploaded = std::make_unique<UploadedModel>();
  uploaded->parts.reserve(model.parts.size());
  const int node_count = static_cast<int>(model.hierarchy.nodes.size());
  for (const asset::ModelPart& part : model.parts) {
    if (part.node < 0 || part.node >= node_count) {
      core::log::Warn("{}: skipping a part of node {} outside the hierarchy",
                      path, part.node);
      continue;
    }
    uploaded->parts.push_back({.node = part.node,
                               .mesh = renderer_.UploadMesh(part.mesh),
                               .bounds = part.mesh.bounds});
  }
  uploaded->hierarchy = std::move(model.hierarchy);
  uploaded->clips = std::move(model.clips);
  uploaded->rest_bounds = model.rest_bounds;

  const double milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  core::log::Info("Loaded model {}: {} nodes, {} parts, {} clips ({:.1f} ms)",
                  path, uploaded->hierarchy.nodes.size(),
                  uploaded->parts.size(), uploaded->clips.size(), milliseconds);
  return models_.emplace(std::move(key), std::move(uploaded))
      .first->second.get();
}

}  // namespace blue::render
