#ifndef BLUE_CORE_PATH_H_
#define BLUE_CORE_PATH_H_

#include <optional>
#include <string>
#include <string_view>

// Lexical operations on UTF-8 paths held in std::string. None of these
// functions touches the file system: they never resolve symlinks and never
// check existence. Both '\' and '/' are accepted as separators in the input;
// every returned path uses '/'.
//
// Recognized path forms:
//   - POSIX absolute:  "/a/b"
//   - Windows drive:   "C:/a" or "c:\a" (absolute); "C:a" is relative to the
//                      current directory of drive C and is not absolute
//   - UNC:             "//server/share/a" (the root is "//server/share/")
//   - relative:        "a/b", "../a"
// Three or more leading separators are treated as one POSIX root.

namespace blue::core {

// Returns `path` in canonical lexical form: "." components removed, ".."
// resolved against the preceding component, repeated separators collapsed,
// and no trailing separator (except on a root). ".." never climbs above the
// root of an absolute path; for relative paths unresolvable leading ".."
// components are kept. A non-empty path that reduces to nothing becomes ".".
// An empty path stays empty.
//   NormalizePath("a/./b/../c//") == "a/c"
//   NormalizePath("C:\\x\\..\\..") == "C:/"
std::string NormalizePath(std::string_view path);

// Returns `base` joined with `relative`, normalized. If `relative` is
// absolute it replaces `base` (a root without a drive, like "/x", keeps the
// drive or UNC share of `base`). An empty side yields the other, normalized.
//   JoinPath("a/b", "../c") == "a/c"
//   JoinPath("C:/a", "D:/b") == "D:/b"
std::string JoinPath(std::string_view base, std::string_view relative);

// Returns the normalized path without its last component. The parent of a
// root is the root itself; the parent of a single relative component, ".",
// ".." or an empty path is "".
//   ParentPath("/a/b/c.txt") == "/a/b"
std::string ParentPath(std::string_view path);

// Returns the last component of the normalized path, or "" for an empty
// path, a root or ".". A trailing separator is ignored.
//   FileName("/a/b/c.tar.gz") == "c.tar.gz"
std::string FileName(std::string_view path);

// Returns FileName() without its last extension. Names without a dot, names
// starting with a dot (".gitignore"), "." and ".." are returned unchanged.
//   FileStem("/a/b/c.tar.gz") == "c.tar"
std::string FileStem(std::string_view path);

// Returns true when `path` is absolute: a POSIX root, a drive with a root
// ("C:/"), or a UNC path. Drive-relative ("C:a") and relative paths are not.
//   IsAbsolutePath("C:\\a") == true, IsAbsolutePath("C:a") == false
bool IsAbsolutePath(std::string_view path);

// Returns the path that leads from `base` to `target`, or nullopt if no
// relative form exists: one path is absolute and the other is not, the roots
// differ (different drive or UNC share; drive letters and UNC names compare
// case-insensitively), or `base` needs unknown ".." steps. Components
// otherwise compare case-sensitively. Equal paths yield ".".
//   RelativePath("/a/assets/t.obj", "/a/scenes") == "../assets/t.obj"
//   RelativePath("C:/a", "D:/a") == std::nullopt
std::optional<std::string> RelativePath(std::string_view target,
                                        std::string_view base);

}  // namespace blue::core

#endif  // BLUE_CORE_PATH_H_
