#include "material/material_file.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/log.h"
#include "core/path.h"
#include "platform/file_system.h"

namespace blue::material {
namespace {

constexpr size_t kMaxMaterialFileSize = 64 * 1024;
constexpr std::string_view kEmbeddedSuffix = ".embedded";
constexpr std::string_view kExtension = ".bmat";

// A texture slot: its .bmat key and the MaterialDesc member it maps to.
struct TextureSlot {
  std::string_view key;
  TextureRef MaterialDesc::* member;
};

constexpr std::array<TextureSlot, 7> kTextureSlots = {{
    {"base_color_texture", &MaterialDesc::base_color_texture},
    {"metallic_roughness_texture", &MaterialDesc::metallic_roughness_texture},
    {"roughness_texture", &MaterialDesc::roughness_texture},
    {"metallic_texture", &MaterialDesc::metallic_texture},
    {"normal_texture", &MaterialDesc::normal_texture},
    {"occlusion_texture", &MaterialDesc::occlusion_texture},
    {"emissive_texture", &MaterialDesc::emissive_texture},
}};

// Returns `text` without leading and trailing whitespace.
std::string_view Trim(std::string_view text) {
  const auto is_space = [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

// Parses exactly kCount finite floats separated by whitespace or commas
// from `text`, or returns nullopt.
template <size_t kCount>
std::optional<std::array<float, kCount>> ParseFloats(std::string_view text) {
  std::array<float, kCount> values{};
  const char* cursor = text.data();
  const char* const end = text.data() + text.size();
  const auto skip_separators = [&] {
    while (cursor != end &&
           (*cursor == ',' ||
            std::isspace(static_cast<unsigned char>(*cursor)) != 0)) {
      ++cursor;
    }
  };
  for (float& value : values) {
    skip_separators();
    const auto [next, ec] = std::from_chars(cursor, end, value);
    if (ec != std::errc{} || !std::isfinite(value)) {
      return std::nullopt;
    }
    cursor = next;
  }
  skip_separators();
  if (cursor != end) {
    return std::nullopt;
  }
  return values;
}

std::optional<bool> ParseBool(std::string_view text) {
  if (text == "true" || text == "1") {
    return true;
  }
  if (text == "false" || text == "0") {
    return false;
  }
  return std::nullopt;
}

std::optional<int> ParseIndex(std::string_view text) {
  int value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() || value < 0) {
    return std::nullopt;
  }
  return value;
}

// Applies one texture slot line ("<slot>" or "<slot>.embedded"). Returns
// false if `key` names no texture slot.
bool ApplyTextureLine(MaterialDesc& desc, std::string_view key,
                      std::string_view value, const std::string& directory,
                      const std::string& path) {
  const bool embedded = key.ends_with(kEmbeddedSuffix);
  if (embedded) {
    key.remove_suffix(kEmbeddedSuffix.size());
  }
  for (const TextureSlot& slot : kTextureSlots) {
    if (slot.key != key) {
      continue;
    }
    TextureRef& ref = desc.*slot.member;
    if (!embedded) {
      ref.path =
          value.empty() ? std::string() : core::JoinPath(directory, value);
    } else if (const std::optional<int> index = ParseIndex(value)) {
      ref.embedded_index = *index;
    } else {
      core::log::Warn("{}: invalid {}{} '{}'", path, key, kEmbeddedSuffix,
                      value);
    }
    return true;
  }
  return false;
}

// Applies one "key = value" `line` read from `path` to `desc`, logging
// anything it cannot use.
void ApplyLine(MaterialDesc& desc, std::string_view line,
               const std::string& directory, const std::string& path) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) {
    core::log::Warn("{}: ignoring malformed line '{}'", path, line);
    return;
  }
  const std::string_view key = Trim(line.substr(0, equals));
  const std::string_view value = Trim(line.substr(equals + 1));
  const auto invalid = [&] {
    core::log::Warn("{}: invalid {} '{}'", path, key, value);
  };
  const auto apply_float = [&](float& target) {
    if (const auto parsed = ParseFloats<1>(value)) {
      target = (*parsed)[0];
    } else {
      invalid();
    }
  };

  if (key == "name") {
    desc.name = value;
  } else if (key == "base_color") {
    if (const auto v = ParseFloats<4>(value)) {
      desc.base_color = {(*v)[0], (*v)[1], (*v)[2], (*v)[3]};
    } else {
      invalid();
    }
  } else if (key == "metallic") {
    apply_float(desc.metallic);
  } else if (key == "roughness") {
    apply_float(desc.roughness);
  } else if (key == "emissive") {
    if (const auto v = ParseFloats<3>(value)) {
      desc.emissive = {(*v)[0], (*v)[1], (*v)[2]};
    } else {
      invalid();
    }
  } else if (key == "emissive_strength") {
    apply_float(desc.emissive_strength);
  } else if (key == "normal_scale") {
    apply_float(desc.normal_scale);
  } else if (key == "occlusion_strength") {
    apply_float(desc.occlusion_strength);
  } else if (key == "uv_scale") {
    if (const auto v = ParseFloats<2>(value)) {
      desc.uv_scale = {(*v)[0], (*v)[1]};
    } else {
      invalid();
    }
  } else if (key == "alpha_mode") {
    if (value == "opaque") {
      desc.alpha_mode = AlphaMode::kOpaque;
    } else if (value == "mask") {
      desc.alpha_mode = AlphaMode::kMask;
    } else {
      invalid();
    }
  } else if (key == "alpha_cutoff") {
    apply_float(desc.alpha_cutoff);
  } else if (key == "double_sided") {
    if (const std::optional<bool> parsed = ParseBool(value)) {
      desc.double_sided = *parsed;
    } else {
      invalid();
    }
  } else if (!ApplyTextureLine(desc, key, value, directory, path)) {
    core::log::Warn("{}: unknown key '{}'", path, key);
  }
}

// Returns how `ref` is written in a .bmat file in `directory`.
std::string StoredPath(const TextureRef& ref, const std::string& directory) {
  std::optional<std::string> relative = core::RelativePath(ref.path, directory);
  return relative.has_value() ? *std::move(relative)
                              : core::NormalizePath(ref.path);
}

}  // namespace

const char* ToString(MaterialFileError error) {
  switch (error) {
    case MaterialFileError::kCannotOpen:
      return "cannot open";
    case MaterialFileError::kWriteFailed:
      return "write failed";
  }
  return "unknown";
}

core::Result<MaterialDesc, MaterialFileError> LoadMaterialFile(
    const std::string& path) {
  using Result = core::Result<MaterialDesc, MaterialFileError>;
  const auto bytes = platform::ReadFile(path, kMaxMaterialFileSize);
  if (!bytes.ok()) {
    return Result::Error(MaterialFileError::kCannotOpen);
  }
  std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                   bytes.value().size());

  MaterialDesc desc;
  desc.name = core::FileStem(path);
  const std::string directory = core::ParentPath(path);
  size_t line_start = 0;
  while (line_start < text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
    const std::string_view line =
        Trim(std::string_view(text).substr(line_start, line_end - line_start));
    line_start = line_end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }
    ApplyLine(desc, line, directory, path);
  }
  return Result::Ok(Sanitize(std::move(desc)));
}

core::Result<void, MaterialFileError> SaveMaterialFile(
    const MaterialDesc& desc, const std::string& path) {
  using Result = core::Result<void, MaterialFileError>;
  const glm::vec4& c = desc.base_color;
  std::string text = std::format(
      "# Project Blue material\n"
      "name = {}\n"
      "base_color = {} {} {} {}\n"
      "metallic = {}\n"
      "roughness = {}\n"
      "emissive = {} {} {}\n"
      "emissive_strength = {}\n"
      "normal_scale = {}\n"
      "occlusion_strength = {}\n"
      "uv_scale = {} {}\n"
      "alpha_mode = {}\n"
      "alpha_cutoff = {}\n"
      "double_sided = {}\n",
      desc.name, c.r, c.g, c.b, c.a, desc.metallic, desc.roughness,
      desc.emissive.r, desc.emissive.g, desc.emissive.b, desc.emissive_strength,
      desc.normal_scale, desc.occlusion_strength, desc.uv_scale.x,
      desc.uv_scale.y, desc.alpha_mode == AlphaMode::kMask ? "mask" : "opaque",
      desc.alpha_cutoff, desc.double_sided);
  const std::string directory = core::ParentPath(path);
  for (const TextureSlot& slot : kTextureSlots) {
    const TextureRef& ref = desc.*slot.member;
    if (ref.empty()) {
      continue;
    }
    text += std::format("{} = {}\n", slot.key, StoredPath(ref, directory));
    if (ref.embedded()) {
      text += std::format("{}{} = {}\n", slot.key, kEmbeddedSuffix,
                          ref.embedded_index);
    }
  }
  const auto written =
      platform::WriteFileAtomic(path, std::as_bytes(std::span(text)));
  if (!written.ok()) {
    core::log::Warn("cannot write material file '{}': {}", path,
                    platform::ToString(written.error()));
    return Result::Error(MaterialFileError::kWriteFailed);
  }
  return Result::Ok();
}

std::string SanitizeMaterialName(std::string_view name) {
  if (name.empty()) {
    return "material";
  }
  std::string result(name);
  for (char& c : result) {
    const auto byte = static_cast<unsigned char>(c);
    const bool safe = (byte < 0x80 && std::isalnum(byte) != 0) || c == '-' ||
                      c == '_' || c == '.';
    if (!safe) {
      c = '_';
    }
  }
  return result;
}

std::string SidecarMaterialPath(std::string_view model_path,
                                std::string_view material_name) {
  return std::format("{}.{}{}", model_path, SanitizeMaterialName(material_name),
                     kExtension);
}

}  // namespace blue::material
