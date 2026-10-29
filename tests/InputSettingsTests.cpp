#include "input/InputSettings.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace blue;

namespace {

// Scratch file in a test-only temp directory, removed on destruction.
class TempFile {
public:
    explicit TempFile(std::string_view name)
        : path_(std::filesystem::temp_directory_path() / "blue_input_settings_tests" / std::string(name))
    {
        std::filesystem::create_directories(path_.parent_path());
    }

    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write(std::string_view text) const
    {
        std::ofstream out(path_, std::ios::trunc);
        out << text;
    }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("missing settings file yields defaults")
{
    const InputSettings settings = loadInputSettings(std::filesystem::temp_directory_path() / "blue_no_such_settings.ini");
    const InputSettings defaults;

    CHECK(settings.mouseSensitivity == defaults.mouseSensitivity);
    CHECK(settings.invertY == defaults.invertY);
}

TEST_CASE("settings round trip through save and load")
{
    const TempFile file("roundtrip.ini");
    const InputSettings saved{.mouseSensitivity = 0.0031f, .invertY = true};

    REQUIRE(saveInputSettings(saved, file.path()));
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(file.path()).concat(".tmp")));

    const InputSettings loaded = loadInputSettings(file.path());
    CHECK(loaded.mouseSensitivity == saved.mouseSensitivity);
    CHECK(loaded.invertY);
}

TEST_CASE("saving creates missing parent directories and overwrites existing files")
{
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "blue_input_settings_tests" / "nested";
    const std::filesystem::path nested = directory / "settings.ini";
    std::filesystem::remove_all(directory);

    REQUIRE(saveInputSettings({.mouseSensitivity = 0.004f, .invertY = false}, nested));
    REQUIRE(saveInputSettings({.mouseSensitivity = 0.005f, .invertY = true}, nested));

    const InputSettings loaded = loadInputSettings(nested);
    CHECK(loaded.mouseSensitivity == 0.005f);
    CHECK(loaded.invertY);

    std::filesystem::remove_all(directory);
}

TEST_CASE("garbage and unknown lines are ignored while valid keys are kept")
{
    const TempFile file("garbage.ini");
    file.write("# comment\n"
               "\n"
               "this is not a setting\n"
               "unknown_key = 5\n"
               "mouse_sensitivity = banana\n"
               "invert_y = true\n"
               "   \t\n"
               "invert_y = maybe\n");

    const InputSettings settings = loadInputSettings(file.path());
    const InputSettings defaults;

    CHECK(settings.mouseSensitivity == defaults.mouseSensitivity); // bad value keeps the default
    CHECK(settings.invertY); // the later bad invert_y does not undo the earlier good one
}

TEST_CASE("out-of-range sensitivity is clamped")
{
    const TempFile file("clamp.ini");

    file.write("mouse_sensitivity = 5\n");
    CHECK(loadInputSettings(file.path()).mouseSensitivity == kMaxMouseSensitivity);

    file.write("mouse_sensitivity = 0.0000001\n");
    CHECK(loadInputSettings(file.path()).mouseSensitivity == kMinMouseSensitivity);

    file.write("mouse_sensitivity = -1\n");
    CHECK(loadInputSettings(file.path()).mouseSensitivity == kMinMouseSensitivity);
}

TEST_CASE("non-finite sensitivity is rejected")
{
    const TempFile file("nonfinite.ini");
    const InputSettings defaults;

    for (const char* text : {"nan", "NaN", "inf", "-inf", "infinity", "1e999"}) {
        file.write(std::string("mouse_sensitivity = ") + text + "\n");
        const InputSettings settings = loadInputSettings(file.path());
        CHECK(std::isfinite(settings.mouseSensitivity));
        CHECK(settings.mouseSensitivity == defaults.mouseSensitivity);
    }
}

TEST_CASE("invert_y accepts true, false, 1 and 0")
{
    const TempFile file("bool.ini");

    file.write("invert_y = true\n");
    CHECK(loadInputSettings(file.path()).invertY);
    file.write("invert_y = false\n");
    CHECK_FALSE(loadInputSettings(file.path()).invertY);
    file.write("invert_y=1\n");
    CHECK(loadInputSettings(file.path()).invertY);
    file.write("  invert_y   =   0  \n");
    CHECK_FALSE(loadInputSettings(file.path()).invertY);
}
