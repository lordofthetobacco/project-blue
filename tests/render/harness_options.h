#ifndef BLUE_TESTS_RENDER_HARNESS_OPTIONS_H_
#define BLUE_TESTS_RENDER_HARNESS_OPTIONS_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/backend.h"

namespace blue::render_test {

// The harness's own command line (everything else goes to doctest):
//   --backend=<name>       repeatable; default: every compiled backend
//   --adapter=hw|software  default hw
//   --update-golden        write the goldens instead of comparing
//   --gpu-validation       RendererOptions::gpu_validation (validation layers /
//                          the D3D12 debug layer with GPU-based validation)
//                          also in Release builds
//   --out=<dir>            rendered images and failure diffs
//   --contact-sheet=<png>  after the tests, tile every golden into <png>
struct HarnessOptions {
  std::vector<render::Backend> backends;
  render::AdapterPreference adapter =
      render::AdapterPreference::kHighPerformance;
  bool update_golden = false;
  bool gpu_validation = false;
  std::string out_dir;
  std::string contact_sheet;
};

// Parses the harness arguments out of `args`, removing them; returns
// nullopt (after printing why) for a malformed one or an unknown backend.
[[nodiscard]] std::optional<HarnessOptions> ParseHarnessOptions(
    std::vector<std::string>& args, std::string default_out_dir);

// The options of this run (set once by main()).
[[nodiscard]] const HarnessOptions& Options();
void SetOptions(HarnessOptions options);

}  // namespace blue::render_test

#endif  // BLUE_TESTS_RENDER_HARNESS_OPTIONS_H_
