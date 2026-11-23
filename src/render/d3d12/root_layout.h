#ifndef BLUE_RENDER_D3D12_ROOT_LAYOUT_H_
#define BLUE_RENDER_D3D12_ROOT_LAYOUT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "render/binding_layout.h"

// The root signatures of the binding_layout.h families as plain data: which
// root parameter holds which bindings. Pure logic, no device (unit tested
// without a GPU); RootSignature (root_signature.h) turns a RootLayout into a
// Direct3D root signature.
//
// Rules (design §3.4 / §4.1), the same for every family:
//   - push constants: root constants at b0 of kPushConstantSpace, as many
//     32-bit values as kMaxPushConstantBytes allows, every stage;
//   - the frame set's uniform buffer (FrameUniforms): a root CBV;
//   - every other set's uniform buffers and textures, and the frame set's
//     textures: one descriptor table per set, the bindings in increasing
//     binding order, contiguous (so a set's descriptors are written as one
//     block, e.g. a material's CBV + 5 SRVs);
//   - every sampler: a table of its own (one sampler descriptor).

namespace blue::render::d3d12 {

enum class RootParamKind : uint8_t {
  kConstants,     // Root constants (push constants).
  kCbv,           // A root constant buffer view.
  kTable,         // A table of CBVs and SRVs.
  kSamplerTable,  // A table of one sampler.
};

// A run of descriptors of one kind in a table.
struct RootRange {
  BindingKind kind = BindingKind::kTexture;
  uint32_t first_register = 0;
  uint32_t count = 0;
  // Index of the range's first descriptor in the table.
  uint32_t table_offset = 0;
};

struct RootParam {
  RootParamKind kind = RootParamKind::kTable;
  uint32_t space = 0;
  // kConstants, kCbv and kSamplerTable: the register.
  uint32_t shader_register = 0;
  // kConstants: the number of 32-bit values.
  uint32_t constant_count = 0;
  // kTable: its ranges (kTable: up to 4; kSamplerTable: none, a single
  // sampler at shader_register).
  std::array<RootRange, 4> ranges{};
  uint32_t range_count = 0;
  // kStageVertex | kStageFragment bits of the bindings it holds.
  uint8_t stages = 0;

  // Descriptors in the table (kTable).
  [[nodiscard]] uint32_t table_size() const;
};

// A root signature's parameters, in root parameter order.
struct RootLayout {
  static constexpr size_t kMaxParams = 12;

  std::array<RootParam, kMaxParams> params{};
  size_t param_count = 0;

  [[nodiscard]] std::span<const RootParam> parameters() const {
    return {params.data(), param_count};
  }
  // Root signature size in 32-bit values (constants 1 each, root
  // descriptors 2, tables 1); Direct3D allows at most 64.
  [[nodiscard]] uint32_t cost_dwords() const;
  // The index of the root parameter of kind `kind` in `space` whose
  // register is `shader_register` (tables: whose ranges include it), or
  // nullopt.
  [[nodiscard]] std::optional<uint32_t> Find(RootParamKind kind,
                                             uint32_t space,
                                             uint32_t shader_register) const;
};

// The root parameter index of the push constants in every layout.
inline constexpr uint32_t kPushConstantsParam = 0;

// Scene programs: the frame set (kFrameBindings) and the pass set
// `pass_set` (one of kPassSets; empty for depth-only programs that only use
// the frame).
[[nodiscard]] RootLayout SceneRootLayout(std::span<const BindingDesc> pass_set);

// Full-screen programs: kMaxFullscreenInputs textures at t0.. of kInputSet
// and kFullscreenSampler.
[[nodiscard]] RootLayout FullscreenRootLayout();

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_ROOT_LAYOUT_H_
