#include "shader/shader_preprocessor.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/path.h"
#include "platform/file_system.h"

namespace blue::shader {

namespace {

constexpr size_t kMaxSourceBytes = 1024 * 1024;
constexpr int kMaxIncludeDepth = 32;

struct StageSuffix {
  std::string_view suffix;
  ShaderStage stage;
};

constexpr StageSuffix kStageSuffixes[] = {
    {".vert.metal", ShaderStage::kVertex},
    {".frag.metal", ShaderStage::kFragment},
    {".comp.metal", ShaderStage::kCompute},
};

// Reads a source file as text. Returns the error text on failure.
core::Result<std::string, std::string> ReadSource(const std::string& path) {
  using ReadResult = core::Result<std::string, std::string>;
  const auto file = platform::ReadFile(path, kMaxSourceBytes);
  if (!file.ok()) {
    return ReadResult::Error("cannot read '" + path +
                             "': " + platform::ToString(file.error()));
  }
  const std::vector<std::byte>& bytes = file.value();
  return ReadResult::Ok(
      std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// Returns the index of the first character at or after `i` that is not a
// space or a tab.
size_t SkipBlanks(std::string_view line, size_t i) {
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  return i;
}

// Returns the name of a `#include "name"` line, or nullopt for any other line
// (including `#include <name>`, which is left to the Metal compiler).
std::optional<std::string_view> QuotedInclude(std::string_view line) {
  constexpr std::string_view kInclude = "include";
  size_t i = SkipBlanks(line, 0);
  if (i == line.size() || line[i] != '#') {
    return std::nullopt;
  }
  i = SkipBlanks(line, i + 1);
  if (line.substr(i, kInclude.size()) != kInclude) {
    return std::nullopt;
  }
  i = SkipBlanks(line, i + kInclude.size());
  if (i == line.size() || line[i] != '"') {
    return std::nullopt;
  }
  const size_t end = line.find('"', i + 1);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  return line.substr(i + 1, end - i - 1);
}

// Returns `#line <line> "<path>"` and a newline, with `path` escaped as a
// string literal.
std::string LineDirective(int line, std::string_view path) {
  std::string directive = "#line " + std::to_string(line) + " \"";
  for (const char c : path) {
    if (c == '"' || c == '\\') {
      directive += '\\';
    }
    directive += c;
  }
  directive += "\"\n";
  return directive;
}

// Builds the flattened source of one root file.
class Flattener {
 public:
  explicit Flattener(std::string root) : root_(std::move(root)) {}

  // Appends `text`, the contents of `path`, with its includes expanded.
  // `depth` is the include nesting of `path` (0 for the root). Returns the
  // error text on failure.
  std::optional<std::string> Append(const std::string& path,
                                    std::string_view text, int depth) {
    source_ += LineDirective(1, path);
    int line_number = 0;
    size_t begin = 0;
    while (begin < text.size()) {
      const size_t newline = text.find('\n', begin);
      const size_t end =
          newline == std::string_view::npos ? text.size() : newline;
      const std::string_view line = text.substr(begin, end - begin);
      begin = end + 1;
      ++line_number;

      const std::optional<std::string_view> name = QuotedInclude(line);
      if (!name) {
        source_.append(line);
        source_ += '\n';
        continue;
      }
      const std::string include_path =
          core::JoinPath(core::ParentPath(path), *name);
      if (AlreadyIncluded(include_path)) {
        source_ += '\n';  // Keeps the line count of `path`.
        continue;
      }
      const std::string location =
          path + ":" + std::to_string(line_number) + ": error: ";
      if (depth >= kMaxIncludeDepth) {
        return location + "includes nest deeper than " +
               std::to_string(kMaxIncludeDepth) + " levels at '" +
               include_path + "'";
      }
      const auto included = ReadSource(include_path);
      if (!included.ok()) {
        return location + included.error();
      }
      dependencies_.push_back(include_path);
      if (auto error = Append(include_path, included.value(), depth + 1)) {
        return error;
      }
      source_ += LineDirective(line_number + 1, path);
    }
    return std::nullopt;
  }

  PreprocessedShader TakeResult(ShaderStage stage) && {
    return {std::move(source_), std::move(dependencies_), stage};
  }

 private:
  bool AlreadyIncluded(const std::string& path) const {
    return path == root_ || std::find(dependencies_.begin(),
                                      dependencies_.end(),
                                      path) != dependencies_.end();
  }

  std::string root_;
  std::string source_;
  std::vector<std::string> dependencies_;
};

}  // namespace

std::optional<ShaderStage> StageForPath(std::string_view path) {
  const std::string name = core::FileName(path);
  for (const StageSuffix& entry : kStageSuffixes) {
    if (name.size() > entry.suffix.size() && name.ends_with(entry.suffix)) {
      return entry.stage;
    }
  }
  return std::nullopt;
}

std::string_view EntryPointName(ShaderStage stage) {
  switch (stage) {
    case ShaderStage::kVertex:
      return "VertexMain";
    case ShaderStage::kFragment:
      return "FragmentMain";
    case ShaderStage::kCompute:
      return "ComputeMain";
  }
  return "";
}

core::Result<PreprocessedShader, ShaderError> PreprocessShader(
    const std::string& path) {
  using PreprocessResult = core::Result<PreprocessedShader, ShaderError>;
  const std::string root = core::NormalizePath(path);
  const std::optional<ShaderStage> stage = StageForPath(root);
  if (!stage) {
    return PreprocessResult::Error(
        {"'" + root +
         "': unsupported shader extension (expected .vert.metal, "
         ".frag.metal or .comp.metal)"});
  }
  const auto source = ReadSource(root);
  if (!source.ok()) {
    return PreprocessResult::Error({source.error()});
  }
  Flattener flattener(root);
  if (auto error = flattener.Append(root, source.value(), 0)) {
    return PreprocessResult::Error({std::move(*error)});
  }
  return PreprocessResult::Ok(std::move(flattener).TakeResult(*stage));
}

}  // namespace blue::shader
