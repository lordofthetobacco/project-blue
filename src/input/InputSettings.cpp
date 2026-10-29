#include "input/InputSettings.hpp"

#include "core/Log.hpp"
#include "core/Path.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace blue {

namespace {

std::string_view trim(std::string_view text)
{
    const auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<float> parseFiniteFloat(std::string_view text)
{
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<bool> parseBool(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "0") {
        return false;
    }
    return std::nullopt;
}

void applyLine(InputSettings& settings, std::string_view line, const std::filesystem::path& path)
{
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
        log::warn("{}: ignoring malformed line '{}'", toUtf8(path), line);
        return;
    }

    const std::string_view key = trim(line.substr(0, equals));
    const std::string_view value = trim(line.substr(equals + 1));

    if (key == "mouse_sensitivity") {
        if (const std::optional<float> parsed = parseFiniteFloat(value)) {
            settings.mouseSensitivity = std::clamp(*parsed, kMinMouseSensitivity, kMaxMouseSensitivity);
        } else {
            log::warn("{}: invalid mouse_sensitivity '{}'", toUtf8(path), value);
        }
    } else if (key == "invert_y") {
        if (const std::optional<bool> parsed = parseBool(value)) {
            settings.invertY = *parsed;
        } else {
            log::warn("{}: invalid invert_y '{}'", toUtf8(path), value);
        }
    } else {
        log::warn("{}: unknown key '{}'", toUtf8(path), key);
    }
}

} // namespace

InputSettings loadInputSettings(const std::filesystem::path& path)
{
    InputSettings settings;

    std::ifstream in(path);
    if (!in) {
        return settings;
    }

    std::string raw;
    while (std::getline(in, raw)) {
        const std::string_view line = trim(raw);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        applyLine(settings, line, path);
    }
    return settings;
}

bool saveInputSettings(const InputSettings& settings, const std::filesystem::path& path)
{
    std::error_code ec;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            log::warn("cannot create settings directory '{}': {}", toUtf8(parent), ec.message());
            return false;
        }
    }

    std::filesystem::path temp = path;
    temp += ".tmp";

    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            log::warn("cannot write settings file '{}'", toUtf8(temp));
            return false;
        }
        out << std::format("# Project Blue input settings\nmouse_sensitivity = {}\ninvert_y = {}\n",
                           settings.mouseSensitivity, settings.invertY);
        out.flush();
        if (!out) {
            out.close();
            std::filesystem::remove(temp, ec);
            log::warn("failed writing settings file '{}'", toUtf8(temp));
            return false;
        }
    }

    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        log::warn("cannot replace settings file '{}'", toUtf8(path));
        return false;
    }
    return true;
}

} // namespace blue
