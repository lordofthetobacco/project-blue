#include "core/path.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>

namespace blue::core {
namespace {

struct UnaryCase {
  const char* input;
  const char* expected;
};

struct JoinCase {
  const char* base;
  const char* relative;
  const char* expected;
};

struct AbsoluteCase {
  const char* input;
  bool expected;
};

struct RelativeCase {
  const char* target;
  const char* base;
  // nullptr means "no relative form".
  const char* expected;
};

TEST_CASE("NormalizePath") {
  const UnaryCase kCases[] = {
      {"", ""},
      {".", "."},
      {"./", "."},
      {"a/./b", "a/b"},
      {"a/b/../c", "a/c"},
      {"a//b///c", "a/b/c"},
      {"a/b/", "a/b"},
      {"a\\b/c", "a/b/c"},
      {"a/..", "."},
      {"a/../..", ".."},
      {"../a", "../a"},
      {"../../a/../b", "../../b"},
      {"/", "/"},
      {"//", "/"},
      {"///a", "/a"},
      {"/a/b/..", "/a"},
      {"/..", "/"},
      {"/../../a", "/a"},
      {"C:\\a\\b", "C:/a/b"},
      {"c:/a/../..", "c:/"},
      {"C:/", "C:/"},
      {"C:", "C:"},
      {"C:a/b", "C:a/b"},
      {"C:../a", "C:../a"},
      {"//server/share/a/../b", "//server/share/b"},
      {"\\\\server\\share", "//server/share/"},
      {"//server/share/../x", "//server/share/x"},
  };
  for (const UnaryCase& c : kCases) {
    INFO("input: ", c.input);
    CHECK(NormalizePath(c.input) == c.expected);
  }
}

TEST_CASE("JoinPath") {
  const JoinCase kCases[] = {
      {"a/b", "c", "a/b/c"},
      {"a/b", "../c", "a/c"},
      {"a/b", ".", "a/b"},
      {"a", "", "a"},
      {"", "b/", "b"},
      {"", "", ""},
      {"..", "..", "../.."},
      {"/a/b", "/c", "/c"},
      {"/a", "../../b", "/b"},
      {"C:\\a", "b\\c", "C:/a/b/c"},
      {"C:/a", "D:/b", "D:/b"},
      {"C:/a", "/b", "C:/b"},
      {"C:/a", "d:x", "d:x"},
      {"C:/a", "C:x", "C:/a/x"},
      {"//srv/sh/a", "../../b", "//srv/sh/b"},
      {"//srv/sh/a", "/b", "//srv/sh/b"},
      {"/a", "C:/b", "C:/b"},
  };
  for (const JoinCase& c : kCases) {
    INFO("base: ", c.base, " relative: ", c.relative);
    CHECK(JoinPath(c.base, c.relative) == c.expected);
  }
}

TEST_CASE("ParentPath") {
  const UnaryCase kCases[] = {
      {"/a/b/c.txt", "/a/b"},
      {"/a", "/"},
      {"/", "/"},
      {"a/b", "a"},
      {"a/b/", "a"},
      {"a\\b\\c", "a/b"},
      {"a", ""},
      {"", ""},
      {"../x", ".."},
      {"..", ""},
      {"C:/a", "C:/"},
      {"C:/", "C:/"},
      {"C:a", "C:"},
      {"//srv/sh/a", "//srv/sh/"},
      {"//srv/sh", "//srv/sh/"},
  };
  for (const UnaryCase& c : kCases) {
    INFO("input: ", c.input);
    CHECK(ParentPath(c.input) == c.expected);
  }
}

TEST_CASE("FileName") {
  const UnaryCase kCases[] = {
      {"/a/b/c.tar.gz", "c.tar.gz"},
      {"/a/b/", "b"},
      {"c.txt", "c.txt"},
      {"/", ""},
      {"", ""},
      {"C:/", ""},
      {"C:a.txt", "a.txt"},
      {"a/..", ""},
      {"..", ".."},
      {"//srv/sh/f.x", "f.x"},
      {"//srv/sh", ""},
  };
  for (const UnaryCase& c : kCases) {
    INFO("input: ", c.input);
    CHECK(FileName(c.input) == c.expected);
  }
}

TEST_CASE("FileStem") {
  const UnaryCase kCases[] = {
      {"/a/b/c.tar.gz", "c.tar"},
      {"C:\\models\\teapot.obj", "teapot"},
      {"/x/.gitignore", ".gitignore"},
      {"noext", "noext"},
      {"dir/file.", "file"},
      {"a/..", ""},
      {"..", ".."},
      {"/", ""},
  };
  for (const UnaryCase& c : kCases) {
    INFO("input: ", c.input);
    CHECK(FileStem(c.input) == c.expected);
  }
}

TEST_CASE("IsAbsolutePath") {
  const AbsoluteCase kCases[] = {
      {"/a", true},    {"/", true},    {"a", false},       {"", false},
      {"../a", false}, {"C:/a", true}, {"C:\\a", true},    {"c:/", true},
      {"C:a", false},  {"C:", false},  {"//s/sh/a", true}, {"\\\\s\\sh", true},
  };
  for (const AbsoluteCase& c : kCases) {
    INFO("input: ", c.input);
    CHECK(IsAbsolutePath(c.input) == c.expected);
  }
}

TEST_CASE("RelativePath") {
  const RelativeCase kCases[] = {
      {"/a/assets/t.obj", "/a/scenes", "../assets/t.obj"},
      {"/a/b", "/a/b", "."},
      {"/a/b/c", "/a", "b/c"},
      {"/a", "/a/b/c", "../.."},
      {"/a/b", "/a/b/../c", "../b"},
      {"C:/proj/assets/t.obj", "c:/proj/scenes", "../assets/t.obj"},
      {"C:\\x\\y", "C:/x", "y"},
      {"a/b", "a/c", "../b"},
      {"../a", "..", "a"},
      {"", "", "."},
      {"//s/sh/a", "//S/SH/b", "../a"},
      {"C:/a", "D:/a", nullptr},
      {"C:/a", "/a", nullptr},
      {"/a", "b", nullptr},
      {"a", "../b", nullptr},
      {"//s/sh1/a", "//s/sh2/a", nullptr},
      {"C:a", "D:a", nullptr},
  };
  for (const RelativeCase& c : kCases) {
    INFO("target: ", c.target, " base: ", c.base);
    const std::optional<std::string> relative = RelativePath(c.target, c.base);
    if (c.expected == nullptr) {
      CHECK_FALSE(relative.has_value());
    } else {
      REQUIRE(relative.has_value());
      CHECK(*relative == c.expected);
    }
  }
}

}  // namespace
}  // namespace blue::core
