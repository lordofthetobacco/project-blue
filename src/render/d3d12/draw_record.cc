#include "render/d3d12/draw_record.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>

#include "render/d3d12/material_system.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/root_layout.h"
#include "render/draw_list.h"
#include "render/scene_view.h"

namespace blue::render::d3d12 {

static_assert(sizeof(glm::mat4) == kDrawConstantCount * 4);

DrawListResources DrawListSource::resources() const {
  return {
      .context = this,
      .mesh_index_count =
          [](const void* context, MeshId mesh, uint32_t& index_count) {
            const auto& source = *static_cast<const DrawListSource*>(context);
            const auto index = static_cast<size_t>(mesh);
            if (index >= source.meshes_.size() ||
                !source.meshes_[index].resident()) {
              return false;
            }
            index_count = source.meshes_[index].index_count();
            return true;
          },
      .resolve_material =
          [](const void* context, MaterialId material,
             MaterialVariant& variant) {
            const auto& source = *static_cast<const DrawListSource*>(context);
            const MaterialId resolved = source.materials_.Resolve(material);
            variant = source.materials_.variant(resolved);
            return resolved;
          },
  };
}

void RecordDrawList(ID3D12GraphicsCommandList* cmd,
                    std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats) {
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ID3D12PipelineState* bound_pipeline = nullptr;
  uint64_t bound_material = 0;
  const Mesh* bound_mesh = nullptr;
  uint32_t pushed_item = UINT32_MAX;
  for (const DrawEntry& entry : entries) {
    const auto variant = static_cast<size_t>(entry.variant);
    ID3D12PipelineState* pipeline = pipelines.pipelines[variant];
    if (pipeline == nullptr) {
      continue;
    }
    if (pipeline != bound_pipeline) {
      // Root arguments survive a pipeline change (same root signature).
      cmd->SetPipelineState(pipeline);
      bound_pipeline = pipeline;
    }
    if (pipelines.bind_material[variant]) {
      const D3D12_GPU_DESCRIPTOR_HANDLE table = materials.table(entry.material);
      if (table.ptr != bound_material) {
        cmd->SetGraphicsRootDescriptorTable(pipelines.material_param, table);
        bound_material = table.ptr;
      }
    }
    const DrawItem& item = view.draws[entry.item];
    const Mesh& mesh = meshes[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(cmd);
      bound_mesh = &mesh;
    }
    if (entry.item != pushed_item) {
      cmd->SetGraphicsRoot32BitConstants(kPushConstantsParam,
                                         kDrawConstantCount, &item.transform,
                                         0);
      pushed_item = entry.item;
    }
    Mesh::DrawRange(cmd, entry.first_index, entry.index_count);
    if (stats != nullptr) {
      ++stats->draw_calls;
      stats->triangles += entry.index_count / 3;
    }
  }
}

}  // namespace blue::render::d3d12
