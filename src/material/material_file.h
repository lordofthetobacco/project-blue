#ifndef BLUE_MATERIAL_MATERIAL_FILE_H_
#define BLUE_MATERIAL_MATERIAL_FILE_H_

#include <string>
#include <string_view>

#include "core/result.h"
#include "material/material.h"

// .bmat material files: UTF-8 "key = value" lines, '#' starts a comment
// line. Example:
//
//   name = steel
//   base_color = 0.8 0.8 0.8 1
//   metallic = 1
//   roughness = 0.35
//   uv_scale = 2 2
//   alpha_mode = opaque            # or "mask"
//   base_color_texture = ../textures/metal032/color.jpg
//   normal_texture = model.glb
//   normal_texture.embedded = 2    # texture embedded in model.glb
//
// Keys: name, base_color (4 floats), metallic, roughness, emissive (3),
// emissive_strength, normal_scale, occlusion_strength, uv_scale (2),
// alpha_mode, alpha_cutoff, double_sided (true/false), and the texture slots
// base_color_texture, metallic_roughness_texture, roughness_texture,
// metallic_texture, normal_texture, occlusion_texture, emissive_texture, each
// with an optional "<slot>.embedded = <index>". Texture paths are stored
// relative to the .bmat file's directory when possible. Optional keys,
// written only when they differ from the defaults: uv_set (5 values, 0 or 1,
// in material::TextureSlot order), normal_encoding (rgb or rg) and
// occlusion_in_metallic_roughness (true/false).

namespace blue::material {

enum class MaterialFileError {
  kCannotOpen,   // The file is missing, too large or cannot be read.
  kWriteFailed,  // The file could not be written.
};

// Returns a short human-readable name for `error`, for log messages.
[[nodiscard]] const char* ToString(MaterialFileError error);

// Loads the .bmat file at `path`. Missing keys keep their MaterialDesc
// defaults; unknown keys, malformed lines and bad values are logged and
// ignored. Texture paths are resolved against the file's directory, so the
// returned refs are absolute when `path` is. The result is Sanitize()d. A
// file without a name key is named after its stem. Returns kCannotOpen if
// the file cannot be read.
[[nodiscard]] core::Result<MaterialDesc, MaterialFileError> LoadMaterialFile(
    const std::string& path);

// Writes `desc` to `path` (atomically, creating directories). Texture paths
// are written relative to the file's directory when a relative form exists,
// otherwise as given. Returns kWriteFailed on failure.
[[nodiscard]] core::Result<void, MaterialFileError> SaveMaterialFile(
    const MaterialDesc& desc, const std::string& path);

// Returns `name` made safe for a file name: every character other than
// ASCII letters, digits, '-', '_' and '.' becomes '_', and an empty name
// becomes "material".
[[nodiscard]] std::string SanitizeMaterialName(std::string_view name);

// Returns the sidecar file that overrides material `material_name` of the
// model at `model_path`: "<model_path>.<SanitizeMaterialName(name)>.bmat",
// e.g. "samples/m16.obj.BaseMetal.bmat".
[[nodiscard]] std::string SidecarMaterialPath(std::string_view model_path,
                                              std::string_view material_name);

}  // namespace blue::material

#endif  // BLUE_MATERIAL_MATERIAL_FILE_H_
