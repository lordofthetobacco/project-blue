#ifndef BLUE_CORE_TEXT_H_
#define BLUE_CORE_TEXT_H_

#include <optional>
#include <string_view>

// Small helpers for reading hand-edited "key = value" text files.

namespace blue::core {

// Returns `text` without leading and trailing whitespace.
std::string_view Trim(std::string_view text);

// Returns the float spelled by all of `text`, or nullopt if it is not a
// number or is not finite.
std::optional<float> ParseFiniteFloat(std::string_view text);

// Returns the int spelled by all of `text` (decimal, optional '-'), or
// nullopt if it is not an integer or does not fit.
std::optional<int> ParseInt(std::string_view text);

// Returns the boolean spelled by `text` ("true"/"false"/"1"/"0", any case), or
// nullopt for anything else.
std::optional<bool> ParseBool(std::string_view text);

}  // namespace blue::core

#endif  // BLUE_CORE_TEXT_H_
