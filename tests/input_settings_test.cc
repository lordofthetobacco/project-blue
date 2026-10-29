#include "input/input_settings.h"

#include <doctest/doctest.h>

#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include "core/path.h"
#include "platform/file_system.h"
#include "tests/scratch_directory.h"

namespace blue::input {

namespace {

// Scratch file in a test-only temp directory, removed on destruction.
class TempFile {
 public:
  explicit TempFile(std::string_view name)
      : dir_("input_settings"), path_(dir_.Child(name)) {}

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

  void write(std::string_view text) const {
    REQUIRE(
        platform::WriteFileAtomic(path_, std::as_bytes(std::span(text))).ok());
  }

 private:
  testing::ScratchDirectory dir_;
  std::string path_;
};

}  // namespace

TEST_CASE("missing settings file yields defaults") {
  const InputSettings settings = LoadInputSettings(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_settings.ini"));
  const InputSettings defaults;

  CHECK(settings.mouse_sensitivity == defaults.mouse_sensitivity);
  CHECK(settings.invert_y == defaults.invert_y);
}

TEST_CASE("settings round trip through save and load") {
  const TempFile file("roundtrip.ini");
  const InputSettings saved{.mouse_sensitivity = 0.0031f, .invert_y = true};

  REQUIRE(SaveInputSettings(saved, file.path()));
  CHECK_FALSE(platform::PathExists(file.path() + ".tmp"));

  const InputSettings loaded = LoadInputSettings(file.path());
  CHECK(loaded.mouse_sensitivity == saved.mouse_sensitivity);
  CHECK(loaded.invert_y);
}

TEST_CASE(
    "saving creates missing parent directories and overwrites existing files") {
  const testing::ScratchDirectory dir("input_settings");
  const std::string nested = dir.Child("nested/settings.ini");

  REQUIRE(SaveInputSettings({.mouse_sensitivity = 0.004f, .invert_y = false},
                            nested));
  REQUIRE(SaveInputSettings({.mouse_sensitivity = 0.005f, .invert_y = true},
                            nested));

  const InputSettings loaded = LoadInputSettings(nested);
  CHECK(loaded.mouse_sensitivity == 0.005f);
  CHECK(loaded.invert_y);
}

TEST_CASE("garbage and unknown lines are ignored while valid keys are kept") {
  const TempFile file("garbage.ini");
  file.write(
      "# comment\n"
      "\n"
      "this is not a setting\n"
      "unknown_key = 5\n"
      "mouse_sensitivity = banana\n"
      "invert_y = true\n"
      "   \t\n"
      "invert_y = maybe\n");

  const InputSettings settings = LoadInputSettings(file.path());
  const InputSettings defaults;

  CHECK(settings.mouse_sensitivity ==
        defaults.mouse_sensitivity);  // bad value keeps the default
  CHECK(settings.invert_y);  // the later bad invert_y does not undo the earlier
                             // good one
}

TEST_CASE("out-of-range sensitivity is clamped") {
  const TempFile file("clamp.ini");

  file.write("mouse_sensitivity = 5\n");
  CHECK(LoadInputSettings(file.path()).mouse_sensitivity ==
        kMaxMouseSensitivity);

  file.write("mouse_sensitivity = 0.0000001\n");
  CHECK(LoadInputSettings(file.path()).mouse_sensitivity ==
        kMinMouseSensitivity);

  file.write("mouse_sensitivity = -1\n");
  CHECK(LoadInputSettings(file.path()).mouse_sensitivity ==
        kMinMouseSensitivity);
}

TEST_CASE("non-finite sensitivity is rejected") {
  const TempFile file("nonfinite.ini");
  const InputSettings defaults;

  for (const char* text : {"nan", "NaN", "inf", "-inf", "infinity", "1e999"}) {
    file.write(std::string("mouse_sensitivity = ") + text + "\n");
    const InputSettings settings = LoadInputSettings(file.path());
    CHECK(std::isfinite(settings.mouse_sensitivity));
    CHECK(settings.mouse_sensitivity == defaults.mouse_sensitivity);
  }
}

TEST_CASE("invert_y accepts true, false, 1 and 0") {
  const TempFile file("bool.ini");

  file.write("invert_y = true\n");
  CHECK(LoadInputSettings(file.path()).invert_y);
  file.write("invert_y = false\n");
  CHECK_FALSE(LoadInputSettings(file.path()).invert_y);
  file.write("invert_y=1\n");
  CHECK(LoadInputSettings(file.path()).invert_y);
  file.write("  invert_y   =   0  \n");
  CHECK_FALSE(LoadInputSettings(file.path()).invert_y);
}

}  // namespace blue::input
