#ifndef BLUE_WEAPON_PARTS_PART_CATALOG_FILE_H_
#define BLUE_WEAPON_PARTS_PART_CATALOG_FILE_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "weapon/parts/part_catalog.h"

// Reading the part catalogue from its text files. Every "*.cfg" file of
// <asset root>/weapons/catalog/ belongs to the catalogue; files are merged in
// file-name order and may refer to each other's parts. Format: UTF-8 text,
// '#' starts a comment, blank lines are ignored, one "key = value" per line
// under a "[part <id>]" or "[preset <id>]" section header. Ids and slot
// names are [a-z0-9_]+. See out/briefs/G0-gunsmith-contract.md for the keys.
// A slot's filter may be empty ("slot.tactical = optional"). An
// "[extend <part id>]" section adds part ids to slots of a part defined in
// any file, one "slot.<name> += id, id" line per slot; ids already accepted
// are skipped. "*.generated.cfg" files (fresh copies `blue-pack samples`
// writes beside an edited file) are not part of the catalogue.

namespace blue::weapon::parts {

// Why a catalogue could not be read: where and what.
struct CatalogError {
  std::string file;
  int line = 0;  // 1-based; 0 when the error concerns the whole file
  std::string message;

  // "file:line: message" (or "file: message" without a line).
  [[nodiscard]] std::string ToString() const;
};

// A slot as written, before part ids are resolved.
struct RawSlotDef {
  std::string name;
  bool required = false;
  std::vector<std::string> accepted;
  int line = 0;
};

// A part as written: every field of `def` except the slots, which are kept
// unresolved in `slots`.
struct RawPartDef {
  PartDef def;
  std::vector<RawSlotDef> slots;
  int line = 0;  // of the section header
};

struct RawPresetEntry {
  std::string path;
  std::string part;
  int line = 0;
};

struct RawPresetDef {
  std::string id;
  std::string name;
  std::string root;
  int root_line = 0;
  std::vector<RawPresetEntry> entries;
  int line = 0;  // of the section header
};

// One "slot.<name> += ids" line of an [extend] section.
struct RawSlotExtension {
  std::string slot;
  std::vector<std::string> accepted;  // not empty
  int line = 0;
};

// An [extend <part id>] section: ids added to slot filters of that part.
struct RawPartExtension {
  std::string part;
  std::vector<RawSlotExtension> slots;
  int line = 0;  // of the section header
};

// One parsed catalogue file. Part references are still ids, so a file may
// use parts another file defines; MergePartCatalogs resolves them.
struct PartCatalogFile {
  std::string file;
  std::vector<RawPartDef> parts;
  std::vector<RawPresetDef> presets;
  std::vector<RawPartExtension> extensions;
};

// Parses the text of one catalogue file. `file_name` is used in errors.
// Checks everything that needs only this file: syntax, ids, unknown or
// repeated keys, numbers and their ranges, and that each key suits the
// part's category. Stops at the first error.
core::Result<PartCatalogFile, CatalogError> ParsePartCatalog(
    std::string_view text, std::string_view file_name);

// Merges parsed files in the given order into a catalogue: rejects a part
// or preset id defined twice (naming both places), resolves slot filters,
// applies the [extend] sections in file order (an unknown part or slot is an
// error at its line), resolves preset parts, and checks that every preset
// can be built (its root is a root part, each part fits its slot's filter,
// no slot is set twice).
core::Result<PartCatalog, CatalogError> MergePartCatalogs(
    std::span<const PartCatalogFile> files);

// Reads, parses and merges every "*.cfg" file directly in `directory` but
// "*.generated.cfg", in file-name order. A missing directory is an error; one
// without catalogue files yields an empty catalogue.
core::Result<PartCatalog, CatalogError> LoadPartCatalog(
    const std::string& directory);

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_PART_CATALOG_FILE_H_
