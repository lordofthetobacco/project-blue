#include "core/text.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace blue::core {

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

std::optional<float> ParseFiniteFloat(std::string_view text) {
  float value = 0.0f;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size() ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> ParseInt(std::string_view text) {
  int value = 0;
  const auto [end, ec] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

std::optional<bool> ParseBool(std::string_view text) {
  std::string lower(text);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "true" || lower == "1") {
    return true;
  }
  if (lower == "false" || lower == "0") {
    return false;
  }
  return std::nullopt;
}

}  // namespace blue::core
