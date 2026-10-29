#pragma once

#include <filesystem>
#include <string>

namespace blue {

// UTF-8 encoding of a path. path::string() uses the ANSI code page on Windows, which
// throws or mangles names outside it.
[[nodiscard]] inline std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

} // namespace blue
