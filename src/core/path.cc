#include "core/path.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blue::core {
namespace {

// A path split into its root and components.
struct ParsedPath {
  // "" for POSIX and plain relative paths, "C:" for a drive, or
  // "//server/share" for UNC.
  std::string root_name;
  // True when the path has a root directory ("/" after the root name).
  bool absolute = false;
  std::vector<std::string> components;
};

bool IsSeparator(char c) { return c == '/' || c == '\\'; }

bool IsAsciiLetter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
      return false;
    }
  }
  return true;
}

// Splits `text` on separators and appends the non-empty segments.
void AppendSegments(std::string_view text,
                    std::vector<std::string>& components) {
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = start;
    while (end < text.size() && !IsSeparator(text[end])) {
      ++end;
    }
    if (end > start) {
      components.emplace_back(text.substr(start, end - start));
    }
    start = end + 1;
  }
}

// Returns the index of the first separator in `path` at or after `from`, or
// path.size() if there is none.
size_t FindSeparator(std::string_view path, size_t from) {
  while (from < path.size() && !IsSeparator(path[from])) {
    ++from;
  }
  return from;
}

// Splits `path` into root and components without simplifying anything.
ParsedPath Parse(std::string_view path) {
  ParsedPath parsed;
  size_t pos = 0;
  if (path.size() >= 2 && IsAsciiLetter(path[0]) && path[1] == ':') {
    parsed.root_name = std::string(path.substr(0, 2));
    pos = 2;
    parsed.absolute = pos < path.size() && IsSeparator(path[pos]);
  } else if (path.size() >= 3 && IsSeparator(path[0]) && IsSeparator(path[1]) &&
             !IsSeparator(path[2])) {
    const size_t server_end = FindSeparator(path, 2);
    size_t share_begin = server_end;
    while (share_begin < path.size() && IsSeparator(path[share_begin])) {
      ++share_begin;
    }
    const size_t share_end = FindSeparator(path, share_begin);
    parsed.root_name = "//" + std::string(path.substr(2, server_end - 2));
    if (share_end > share_begin) {
      parsed.root_name +=
          "/" + std::string(path.substr(share_begin, share_end - share_begin));
    }
    parsed.absolute = true;
    pos = share_end;
  } else if (!path.empty() && IsSeparator(path[0])) {
    parsed.absolute = true;
  }
  AppendSegments(path.substr(pos), parsed.components);
  return parsed;
}

// Removes "." and resolves ".." in place.
void Simplify(ParsedPath& parsed) {
  std::vector<std::string> kept;
  for (std::string& component : parsed.components) {
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      if (!kept.empty() && kept.back() != "..") {
        kept.pop_back();
      } else if (!parsed.absolute) {
        kept.push_back(std::move(component));
      }
      continue;
    }
    kept.push_back(std::move(component));
  }
  parsed.components = std::move(kept);
}

// Returns the generic string form. A path that formats to nothing is ""
// when `was_empty_input` and "." otherwise.
std::string Format(const ParsedPath& parsed, bool was_empty_input) {
  std::string result = parsed.root_name;
  if (parsed.absolute) {
    result += '/';
  }
  for (size_t i = 0; i < parsed.components.size(); ++i) {
    if (i > 0) {
      result += '/';
    }
    result += parsed.components[i];
  }
  if (result.empty() && !was_empty_input) {
    return ".";
  }
  return result;
}

ParsedPath ParseAndSimplify(std::string_view path) {
  ParsedPath parsed = Parse(path);
  Simplify(parsed);
  return parsed;
}

}  // namespace

std::string NormalizePath(std::string_view path) {
  return Format(ParseAndSimplify(path), path.empty());
}

std::string JoinPath(std::string_view base, std::string_view relative) {
  if (relative.empty()) {
    return NormalizePath(base);
  }
  if (base.empty()) {
    return NormalizePath(relative);
  }
  ParsedPath joined = Parse(base);
  ParsedPath rel = Parse(relative);
  if (rel.absolute) {
    if (rel.root_name.empty()) {
      rel.root_name = joined.root_name;
    }
    Simplify(rel);
    return Format(rel, false);
  }
  if (!rel.root_name.empty() &&
      !EqualsIgnoreAsciiCase(rel.root_name, joined.root_name)) {
    Simplify(rel);
    return Format(rel, false);
  }
  for (std::string& component : rel.components) {
    joined.components.push_back(std::move(component));
  }
  Simplify(joined);
  return Format(joined, false);
}

std::string ParentPath(std::string_view path) {
  ParsedPath parsed = ParseAndSimplify(path);
  if (parsed.components.empty()) {
    return parsed.absolute ? Format(parsed, false) : std::string();
  }
  if (parsed.components.size() == 1 && parsed.components[0] == "..") {
    return std::string();
  }
  parsed.components.pop_back();
  if (parsed.components.empty() && !parsed.absolute) {
    return parsed.root_name;
  }
  return Format(parsed, false);
}

std::string FileName(std::string_view path) {
  const ParsedPath parsed = ParseAndSimplify(path);
  if (parsed.components.empty()) {
    return std::string();
  }
  return parsed.components.back();
}

std::string FileStem(std::string_view path) {
  std::string name = FileName(path);
  if (name == "..") {
    return name;
  }
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    return name;
  }
  name.resize(dot);
  return name;
}

bool IsAbsolutePath(std::string_view path) { return Parse(path).absolute; }

std::optional<std::string> RelativePath(std::string_view target,
                                        std::string_view base) {
  const ParsedPath to = ParseAndSimplify(target);
  const ParsedPath from = ParseAndSimplify(base);
  if (to.absolute != from.absolute ||
      !EqualsIgnoreAsciiCase(to.root_name, from.root_name)) {
    return std::nullopt;
  }
  size_t common = 0;
  while (common < to.components.size() && common < from.components.size() &&
         to.components[common] == from.components[common]) {
    ++common;
  }
  ParsedPath result;
  for (size_t i = common; i < from.components.size(); ++i) {
    if (from.components[i] == "..") {
      return std::nullopt;
    }
    result.components.push_back("..");
  }
  for (size_t i = common; i < to.components.size(); ++i) {
    result.components.push_back(to.components[i]);
  }
  return Format(result, false);
}

}  // namespace blue::core
