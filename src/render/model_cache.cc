#include "render/model_cache.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "asset/animated_model.h"
#include "asset/model_importer.h"
#include "core/log.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "render/mesh_cache.h"

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
  renderer_.PreloadTextures(model.materials);
  for (const material::MaterialDesc& material : model.materials) {
    uploaded->materials.push_back(renderer_.CreateMaterial(material));
  }

  // Resolve every part's submeshes first: the spans into part_submeshes are
  // taken once it no longer grows.
  const int node_count = static_cast<int>(model.hierarchy.nodes.size());
  std::vector<const asset::ModelPart*> kept_parts;
  for (const asset::ModelPart& part : model.parts) {
    if (part.node < 0 || part.node >= node_count) {
      core::log::Warn("{}: skipping a part of node {} outside the hierarchy",
                      path, part.node);
      continue;
    }
    kept_parts.push_back(&part);
    uploaded->part_submeshes.push_back(
        ResolveSubmeshes(part.mesh.submeshes, uploaded->materials));
  }
  uploaded->parts.reserve(kept_parts.size());
  for (size_t i = 0; i < kept_parts.size(); ++i) {
    const asset::ModelPart& part = *kept_parts[i];
    uploaded->parts.push_back({.node = part.node,
                               .mesh = renderer_.UploadMesh(part.mesh),
                               .bounds = part.mesh.bounds,
                               .submeshes = uploaded->part_submeshes[i]});
  }
  uploaded->hierarchy = std::move(model.hierarchy);
  uploaded->clips = std::move(model.clips);
  uploaded->rest_bounds = model.rest_bounds;

  const double milliseconds = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
  core::log::Info(
      "Loaded model {}: {} nodes, {} parts, {} materials, {} clips ({:.1f} ms "
      "including textures)",
      path, uploaded->hierarchy.nodes.size(), uploaded->parts.size(),
      uploaded->materials.size(), uploaded->clips.size(), milliseconds);
  return models_.emplace(std::move(key), std::move(uploaded))
      .first->second.get();
}

}  // namespace blue::render
