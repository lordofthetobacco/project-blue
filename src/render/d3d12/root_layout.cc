#include "render/d3d12/root_layout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "core/log.h"
#include "render/binding_layout.h"

namespace blue::render::d3d12 {

namespace {

constexpr size_t kMaxSetBindings = 16;

RootParam& AddParam(RootLayout& layout) {
  if (layout.param_count == RootLayout::kMaxParams) {
    core::log::Fatal("Root layout has more than {} parameters",
                     RootLayout::kMaxParams);
  }
  return layout.params[layout.param_count++];
}

void AddPushConstants(RootLayout& layout) {
  RootParam& param = AddParam(layout);
  param.kind = RootParamKind::kConstants;
  param.space = kPushConstantSpace;
  param.shader_register = 0;
  param.constant_count = kMaxPushConstantBytes / 4;
  param.stages = kStageVertex | kStageFragment;
}

// Adds the parameters of set `space` holding `bindings`: with
// `root_uniforms`, its uniform buffers become root CBVs; its other
// non-sampler bindings one table; each sampler a table of its own.
void AddSet(RootLayout& layout, uint32_t space,
            std::span<const BindingDesc> bindings, bool root_uniforms) {
  if (bindings.size() > kMaxSetBindings) {
    core::log::Fatal("Set {} has more than {} bindings", space,
                     kMaxSetBindings);
  }
  std::array<BindingDesc, kMaxSetBindings> sorted{};
  std::copy(bindings.begin(), bindings.end(), sorted.begin());
  std::sort(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(bindings.size()),
            [](const BindingDesc& a, const BindingDesc& b) {
              return a.binding < b.binding;
            });
  const std::span<const BindingDesc> ordered(sorted.data(), bindings.size());

  RootParam table;
  table.kind = RootParamKind::kTable;
  table.space = space;
  uint32_t table_size = 0;
  for (const BindingDesc& binding : ordered) {
    if (binding.kind == BindingKind::kSampler) {
      continue;
    }
    if (binding.kind == BindingKind::kUniformBuffer && root_uniforms) {
      RootParam& cbv = AddParam(layout);
      cbv.kind = RootParamKind::kCbv;
      cbv.space = space;
      cbv.shader_register = binding.binding;
      cbv.stages = binding.stages;
      continue;
    }
    // Extends the last range when kind and register continue it.
    RootRange* last =
        table.range_count > 0 ? &table.ranges[table.range_count - 1] : nullptr;
    if (last != nullptr && last->kind == binding.kind &&
        last->first_register + last->count == binding.binding) {
      ++last->count;
    } else {
      if (table.range_count == table.ranges.size()) {
        core::log::Fatal("Set {} needs more than {} descriptor ranges", space,
                         table.ranges.size());
      }
      table.ranges[table.range_count++] = {.kind = binding.kind,
                                           .first_register = binding.binding,
                                           .count = 1,
                                           .table_offset = table_size};
    }
    ++table_size;
    table.stages |= binding.stages;
  }
  if (table.range_count > 0) {
    AddParam(layout) = table;
  }
  for (const BindingDesc& binding : ordered) {
    if (binding.kind != BindingKind::kSampler) {
      continue;
    }
    RootParam& sampler = AddParam(layout);
    sampler.kind = RootParamKind::kSamplerTable;
    sampler.space = space;
    sampler.shader_register = binding.binding;
    sampler.stages = binding.stages;
  }
}

}  // namespace

uint32_t RootParam::table_size() const {
  uint32_t size = 0;
  for (uint32_t i = 0; i < range_count; ++i) {
    size = std::max(size, ranges[i].table_offset + ranges[i].count);
  }
  return size;
}

uint32_t RootLayout::cost_dwords() const {
  uint32_t cost = 0;
  for (const RootParam& param : parameters()) {
    switch (param.kind) {
      case RootParamKind::kConstants:
        cost += param.constant_count;
        break;
      case RootParamKind::kCbv:
        cost += 2;
        break;
      case RootParamKind::kTable:
      case RootParamKind::kSamplerTable:
        cost += 1;
        break;
    }
  }
  return cost;
}

std::optional<uint32_t> RootLayout::Find(RootParamKind kind, uint32_t space,
                                         uint32_t shader_register) const {
  for (uint32_t i = 0; i < param_count; ++i) {
    const RootParam& param = params[i];
    if (param.kind != kind || param.space != space) {
      continue;
    }
    if (kind != RootParamKind::kTable) {
      if (param.shader_register == shader_register) {
        return i;
      }
      continue;
    }
    for (uint32_t r = 0; r < param.range_count; ++r) {
      const RootRange& range = param.ranges[r];
      if (shader_register >= range.first_register &&
          shader_register < range.first_register + range.count) {
        return i;
      }
    }
  }
  return std::nullopt;
}

RootLayout SceneRootLayout(std::span<const BindingDesc> pass_set) {
  RootLayout layout;
  AddPushConstants(layout);
  AddSet(layout, kFrameSet, kFrameBindings, /*root_uniforms=*/true);
  if (!pass_set.empty()) {
    AddSet(layout, kPassSet, pass_set, /*root_uniforms=*/false);
  }
  return layout;
}

RootLayout FullscreenRootLayout() {
  std::array<BindingDesc, kMaxFullscreenInputs + 1> bindings{};
  for (uint32_t i = 0; i < kMaxFullscreenInputs; ++i) {
    bindings[i] = {.name = "input",
                   .binding = i,
                   .kind = BindingKind::kTexture,
                   .stages = kStageFragment};
  }
  bindings[kMaxFullscreenInputs] = kFullscreenSampler;
  RootLayout layout;
  AddPushConstants(layout);
  AddSet(layout, kInputSet, bindings, /*root_uniforms=*/false);
  return layout;
}

}  // namespace blue::render::d3d12
