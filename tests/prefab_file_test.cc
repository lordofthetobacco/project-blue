#include "prefab/prefab_file.h"

#include <SDL3/SDL_iostream.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "asset/mesh_data.h"
#include "core/path.h"
#include "platform/file_system.h"
#include "platform/mapped_file.h"
#include "prefab/prefab.h"
#include "scene/binary_stream.h"
#include "tests/prefab_samples.h"
#include "tests/scratch_directory.h"

namespace blue::prefab {
namespace {

// Header layout (see prefab_file.h).
constexpr size_t kVersionOffset = 8;
constexpr size_t kFileSizeOffset = 12;
constexpr size_t kChunkCountOffset = 16;
constexpr size_t kStrideOffset = 20;
constexpr size_t kHeaderCrcOffset = 28;
constexpr size_t kHeaderSize = 32;
constexpr size_t kTocEntrySize = 16;
constexpr uint32_t kBulk = scene::MakeFourcc('B', 'U', 'L', 'K');

uint32_t GetU32(std::span<const std::byte> file, size_t offset) {
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value |= std::to_integer<uint32_t>(file[offset + i]) << (8 * i);
  }
  return value;
}

void PutU32(std::vector<std::byte>& file, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    file[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
  }
}

struct Entry {
  uint32_t fourcc;
  uint32_t offset;
  uint32_t size;
};

std::vector<Entry> Toc(std::span<const std::byte> file) {
  std::vector<Entry> entries;
  const uint32_t count = GetU32(file, kChunkCountOffset);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = kHeaderSize + i * kTocEntrySize;
    entries.push_back(
        {GetU32(file, at), GetU32(file, at + 4), GetU32(file, at + 8)});
  }
  return entries;
}

// Recomputes every chunk CRC and the header CRC, so a deliberately modified
// file reaches the decoder instead of failing the checksum.
void FixChecksums(std::vector<std::byte>& file) {
  const uint32_t count = GetU32(file, kChunkCountOffset);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = kHeaderSize + i * kTocEntrySize;
    if (at + kTocEntrySize > file.size()) {
      break;
    }
    const uint32_t offset = GetU32(file, at + 4);
    const uint32_t size = GetU32(file, at + 8);
    if (size_t{offset} + size <= file.size()) {
      PutU32(
          file, at + 12,
          scene::Crc32(std::span<const std::byte>(file).subspan(offset, size)));
    }
  }
  std::vector<std::byte> checked(file.begin(), file.begin() + kHeaderCrcOffset);
  const size_t toc_end = kHeaderSize + size_t{count} * kTocEntrySize;
  if (toc_end <= file.size()) {
    checked.insert(checked.end(), file.begin() + kHeaderSize,
                   file.begin() + static_cast<ptrdiff_t>(toc_end));
  }
  PutU32(file, kHeaderCrcOffset, scene::Crc32(checked));
}

std::vector<std::byte> Encode(const Prefab& prefab) {
  auto encoded = EncodePrefab(prefab, "");
  REQUIRE(encoded.ok());
  return std::move(encoded).value();
}

core::Result<Prefab, PrefabError> Decode(std::vector<std::byte> file,
                                         bool verify_bulk = false) {
  return DecodePrefab(std::move(file), "",
                      {.verify_bulk_checksum = verify_bulk});
}

PrefabError DecodeError(std::vector<std::byte> file) {
  auto decoded = Decode(std::move(file));
  REQUIRE_FALSE(decoded.ok());
  return decoded.error();
}

bool SameBytes(std::span<const std::byte> a, std::span<const std::byte> b) {
  return std::ranges::equal(a, b);
}

bool IsAligned(const void* pointer) {
  return std::bit_cast<uintptr_t>(pointer) % kBulkAlignment == 0;
}

bool SameTransform(const animation::Transform& a,
                   const animation::Transform& b) {
  return a.translation == b.translation && a.rotation == b.rotation &&
         a.scale == b.scale;
}

// Checks that every field of `b` matches `a`, bulk data by content.
void CheckSamePrefab(const Prefab& a, const Prefab& b) {
  CHECK(a.bounds.min == b.bounds.min);
  CHECK(a.bounds.max == b.bounds.max);
  REQUIRE(a.hierarchy.nodes.size() == b.hierarchy.nodes.size());
  for (size_t i = 0; i < a.hierarchy.nodes.size(); ++i) {
    CHECK(a.hierarchy.nodes[i].name == b.hierarchy.nodes[i].name);
    CHECK(a.hierarchy.nodes[i].parent == b.hierarchy.nodes[i].parent);
    CHECK(SameTransform(a.hierarchy.nodes[i].rest, b.hierarchy.nodes[i].rest));
  }
  REQUIRE(a.meshes.size() == b.meshes.size());
  for (size_t i = 0; i < a.meshes.size(); ++i) {
    const Mesh& x = a.meshes[i];
    const Mesh& y = b.meshes[i];
    CHECK(x.name == y.name);
    CHECK(x.node == y.node);
    CHECK(x.bounds.min == y.bounds.min);
    CHECK(x.bounds.max == y.bounds.max);
    CHECK(SameBytes(std::as_bytes(x.vertices), std::as_bytes(y.vertices)));
    CHECK(SameBytes(std::as_bytes(x.indices), std::as_bytes(y.indices)));
    REQUIRE(x.submeshes.size() == y.submeshes.size());
    for (size_t s = 0; s < x.submeshes.size(); ++s) {
      CHECK(x.submeshes[s].first_index == y.submeshes[s].first_index);
      CHECK(x.submeshes[s].index_count == y.submeshes[s].index_count);
      CHECK(x.submeshes[s].material == y.submeshes[s].material);
    }
  }
  REQUIRE(a.materials.size() == b.materials.size());
  for (size_t i = 0; i < a.materials.size(); ++i) {
    const Material& x = a.materials[i];
    const Material& y = b.materials[i];
    CHECK(x.name == y.name);
    CHECK(x.base_color == y.base_color);
    CHECK(x.metallic == y.metallic);
    CHECK(x.roughness == y.roughness);
    CHECK(x.emissive == y.emissive);
    CHECK(x.emissive_strength == y.emissive_strength);
    CHECK(x.normal_scale == y.normal_scale);
    CHECK(x.occlusion_strength == y.occlusion_strength);
    CHECK(x.uv_scale == y.uv_scale);
    CHECK(x.alpha_mode == y.alpha_mode);
    CHECK(x.alpha_cutoff == y.alpha_cutoff);
    CHECK(x.double_sided == y.double_sided);
    CHECK(x.textures == y.textures);
    CHECK(x.uv_sets == y.uv_sets);
  }
  REQUIRE(a.textures.size() == b.textures.size());
  for (size_t i = 0; i < a.textures.size(); ++i) {
    const Texture& x = a.textures[i];
    const Texture& y = b.textures[i];
    CHECK(x.name == y.name);
    CHECK(x.format == y.format);
    CHECK(x.width == y.width);
    CHECK(x.height == y.height);
    REQUIRE(x.mip_count() == y.mip_count());
    for (size_t m = 0; m < x.mips.size(); ++m) {
      CHECK(x.mips[m].width == y.mips[m].width);
      CHECK(x.mips[m].height == y.mips[m].height);
      CHECK(SameBytes(x.mips[m].data, y.mips[m].data));
    }
  }
  REQUIRE(a.clips.size() == b.clips.size());
  for (size_t i = 0; i < a.clips.size(); ++i) {
    const animation::Clip& x = a.clips[i];
    const animation::Clip& y = b.clips[i];
    CHECK(x.name == y.name);
    CHECK(x.duration == y.duration);
    CHECK(x.loop == y.loop);
    CHECK(x.events == y.events);
    REQUIRE(x.channels.size() == y.channels.size());
    for (size_t c = 0; c < x.channels.size(); ++c) {
      CHECK(x.channels[c].node == y.channels[c].node);
      CHECK(x.channels[c].path == y.channels[c].path);
      CHECK(x.channels[c].interpolation == y.channels[c].interpolation);
      CHECK(x.channels[c].times == y.channels[c].times);
      CHECK(x.channels[c].values == y.channels[c].values);
    }
  }
  REQUIRE(a.sockets.size() == b.sockets.size());
  for (size_t i = 0; i < a.sockets.size(); ++i) {
    CHECK(a.sockets[i].name == b.sockets[i].name);
    CHECK(a.sockets[i].node == b.sockets[i].node);
    CHECK(SameTransform(a.sockets[i].local, b.sockets[i].local));
  }
  REQUIRE(a.lights.size() == b.lights.size());
  for (size_t i = 0; i < a.lights.size(); ++i) {
    CHECK(a.lights[i].name == b.lights[i].name);
    CHECK(a.lights[i].node == b.lights[i].node);
    CHECK(a.lights[i].position == b.lights[i].position);
    CHECK(a.lights[i].color == b.lights[i].color);
    CHECK(a.lights[i].intensity == b.lights[i].intensity);
    CHECK(a.lights[i].range == b.lights[i].range);
  }
  REQUIRE(a.children.size() == b.children.size());
  for (size_t i = 0; i < a.children.size(); ++i) {
    CHECK(a.children[i].path == b.children[i].path);
    CHECK(a.children[i].node == b.children[i].node);
    CHECK(SameTransform(a.children[i].local, b.children[i].local));
  }
  const Physics& p = a.physics;
  const Physics& q = b.physics;
  CHECK(p.kind == q.kind);
  CHECK(p.center == q.center);
  CHECK(p.rotation == q.rotation);
  CHECK(p.half_extents == q.half_extents);
  CHECK(p.radius == q.radius);
  CHECK(p.half_height == q.half_height);
  CHECK(p.dynamic == q.dynamic);
  CHECK(p.mass == q.mass);
  CHECK(p.friction == q.friction);
  CHECK(p.restitution == q.restitution);
  CHECK(SameBytes(p.cooked_shape, q.cooked_shape));
  CHECK(a.edit.source_model == b.edit.source_model);
  CHECK(a.edit.import_settings == b.edit.import_settings);
  CHECK(a.edit.texture_sources == b.edit.texture_sources);
}

// Returns a prefab of roughly `vertex_count` * 64 bytes of mesh data plus a
// 2048^2 BC7 texture with mips, for timing.
Prefab MakeLargePrefab(size_t vertex_count) {
  Prefab prefab;
  std::vector<asset::Vertex> vertices(vertex_count);
  for (size_t i = 0; i < vertices.size(); ++i) {
    vertices[i].position = {static_cast<float>(i % 1000), 0.0f,
                            static_cast<float>(i / 1000)};
  }
  std::vector<asset::Index> indices(vertex_count * 3);
  for (size_t i = 0; i < indices.size(); ++i) {
    indices[i] = static_cast<asset::Index>((i * 7919) % vertex_count);
  }
  prefab.materials.push_back({.name = "m"});
  Mesh mesh{.name = "big"};
  mesh.vertices = prefab.storage.StoreArray<asset::Vertex>(std::span(vertices));
  mesh.indices = prefab.storage.StoreArray<asset::Index>(std::span(indices));
  mesh.submeshes = {{.first_index = 0,
                     .index_count = static_cast<uint32_t>(indices.size()),
                     .material = 0}};
  prefab.meshes.push_back(std::move(mesh));
  prefab.textures.push_back(testing::MakePatternTexture(
      prefab, "big", TextureFormat::kBc7Srgb, 2048, 2048, 3));
  return prefab;
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

TEST_CASE("Every chunk type survives an encode/decode round trip") {
  const Prefab sample = testing::MakeSamplePrefab();
  auto decoded = Decode(Encode(sample));
  REQUIRE(decoded.ok());
  CheckSamePrefab(sample, decoded.value());
  // One storage block: the file itself, viewed in place.
  CHECK(decoded.value().storage.block_count() == 1);
}

TEST_CASE("An empty prefab round trips") {
  auto decoded = Decode(Encode(Prefab{}));
  REQUIRE(decoded.ok());
  CheckSamePrefab(Prefab{}, decoded.value());
  CHECK(decoded.value().physics.kind == ColliderKind::kNone);
}

TEST_CASE(
    "Encoding is deterministic and re-encoding a loaded prefab is "
    "byte-identical") {
  const std::vector<std::byte> first = Encode(testing::MakeSamplePrefab());
  CHECK(first == Encode(testing::MakeSamplePrefab()));

  auto decoded = Decode(first);
  REQUIRE(decoded.ok());
  CHECK(Encode(decoded.value()) == first);

  // A copy keeps the bulk alive and encodes the same after the original goes.
  std::optional<Prefab> original(std::move(decoded).value());
  const Prefab copy = *original;
  original.reset();
  CHECK(Encode(copy) == first);

  // Editing metadata only and saving keeps the bulk bytes untouched.
  Prefab edited = copy;
  edited.materials[0].roughness = 0.9f;
  auto reloaded = Decode(Encode(edited));
  REQUIRE(reloaded.ok());
  CHECK(reloaded.value().materials[0].roughness == 0.9f);
  CHECK(SameBytes(reloaded.value().textures[0].mips[0].data,
                  copy.textures[0].mips[0].data));
}

TEST_CASE("Bulk data is 16-byte aligned in the file and in memory") {
  const std::vector<std::byte> file = Encode(testing::MakeSamplePrefab());
  for (const Entry& entry : Toc(file)) {
    CHECK(entry.offset % kBulkAlignment == 0);
  }
  auto decoded = Decode(file);
  REQUIRE(decoded.ok());
  const Prefab& prefab = decoded.value();
  CHECK(IsAligned(prefab.meshes[0].vertices.data()));
  CHECK(IsAligned(prefab.meshes[0].indices.data()));
  CHECK(IsAligned(prefab.physics.cooked_shape.data()));
  for (const Texture& texture : prefab.textures) {
    for (const TextureMip& mip : texture.mips) {
      CHECK(IsAligned(mip.data.data()));
    }
  }
}

TEST_CASE("SavePrefab and LoadPrefab store paths relative to the file") {
  const testing::ScratchDirectory scratch("pbo");
  Prefab prefab = testing::MakeSamplePrefab();
  prefab.children[0].path = scratch.Child("parts/sight.pbo");
  prefab.edit.source_model = scratch.Child("sources/glock.gltf");
  prefab.edit.texture_sources = {{scratch.Child("sources/albedo.png")}};
  const std::string path = scratch.Child("prefabs/glock.pbo");
  REQUIRE(SavePrefab(prefab, path).ok());

  auto loaded = LoadPrefab(path);
  REQUIRE(loaded.ok());
  CheckSamePrefab(prefab, loaded.value());

  // Re-saving the loaded prefab to its own path reproduces the file.
  const auto bytes = platform::ReadFile(path, kMaxPrefabFileSize);
  REQUIRE(bytes.ok());
  const std::string resaved = scratch.Child("prefabs/again.pbo");
  REQUIRE(SavePrefab(loaded.value(), resaved).ok());
  const auto resaved_bytes = platform::ReadFile(resaved, kMaxPrefabFileSize);
  REQUIRE(resaved_bytes.ok());
  CHECK(resaved_bytes.value() == bytes.value());

  // The stored form is relative: a moved folder resolves against its new
  // location.
  const std::string moved = scratch.Child("elsewhere/deep/glock.pbo");
  REQUIRE(platform::WriteFileAtomic(moved, bytes.value()).ok());
  auto from_moved = LoadPrefab(moved);
  REQUIRE(from_moved.ok());
  CHECK(from_moved.value().children[0].path ==
        scratch.Child("elsewhere/parts/sight.pbo"));
  CHECK(from_moved.value().edit.source_model ==
        scratch.Child("elsewhere/sources/glock.gltf"));
}

TEST_CASE("LoadPrefab and SavePrefab report I/O and validation errors") {
  const testing::ScratchDirectory scratch("pbo_errors");
  auto missing = LoadPrefab(scratch.Child("missing.pbo"));
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error() == PrefabError::kCannotOpen);

  Prefab invalid = testing::MakeSamplePrefab();
  invalid.meshes[0].node = 9;
  const std::string path = scratch.Child("invalid.pbo");
  auto saved = SavePrefab(invalid, path);
  REQUIRE_FALSE(saved.ok());
  CHECK(saved.error() == PrefabError::kInvalidPrefab);
  CHECK_FALSE(platform::PathExists(path));
  CHECK(std::string(ToString(PrefabError::kChecksumMismatch)) ==
        "checksum mismatch");
}

TEST_CASE("Unknown chunks are skipped") {
  std::vector<std::byte> file = Encode(testing::MakeSamplePrefab());
  const uint32_t count = GetU32(file, kChunkCountOffset);
  const size_t toc_end = kHeaderSize + size_t{count} * kTocEntrySize;

  // Insert one TOC entry; every chunk moves by 16 bytes, keeping alignment.
  std::vector<std::byte> grown(file.begin(),
                               file.begin() + static_cast<ptrdiff_t>(toc_end));
  grown.resize(grown.size() + kTocEntrySize);
  grown.insert(grown.end(), file.begin() + static_cast<ptrdiff_t>(toc_end),
               file.end());
  for (uint32_t i = 0; i < count; ++i) {
    const size_t at = kHeaderSize + i * kTocEntrySize;
    PutU32(grown, at + 4, GetU32(grown, at + 4) + kTocEntrySize);
  }
  const std::vector<std::byte> extra = testing::PatternBytes(40, 5);
  const size_t extra_offset = (grown.size() + 15) / 16 * 16;
  grown.resize(extra_offset);
  grown.insert(grown.end(), extra.begin(), extra.end());
  PutU32(grown, toc_end, scene::MakeFourcc('F', 'U', 'T', 'R'));
  PutU32(grown, toc_end + 4, static_cast<uint32_t>(extra_offset));
  PutU32(grown, toc_end + 8, static_cast<uint32_t>(extra.size()));
  PutU32(grown, kChunkCountOffset, count + 1);
  PutU32(grown, kFileSizeOffset, static_cast<uint32_t>(grown.size()));
  FixChecksums(grown);

  auto decoded = Decode(grown, /*verify_bulk=*/true);
  REQUIRE(decoded.ok());
  CheckSamePrefab(testing::MakeSamplePrefab(), decoded.value());
}

TEST_CASE("Version and vertex layout checks") {
  const std::vector<std::byte> valid = Encode(testing::MakeSamplePrefab());

  std::vector<std::byte> newer = valid;
  PutU32(newer, kVersionOffset, kPrefabFormatVersion + 1);
  FixChecksums(newer);
  CHECK(DecodeError(newer) == PrefabError::kUnsupportedVersion);

  std::vector<std::byte> zero = valid;
  PutU32(zero, kVersionOffset, 0);
  FixChecksums(zero);
  CHECK(DecodeError(zero) == PrefabError::kCorrupt);

  std::vector<std::byte> stride = valid;
  PutU32(stride, kStrideOffset, sizeof(asset::Vertex) - 8);
  FixChecksums(stride);
  CHECK(DecodeError(stride) == PrefabError::kUnsupportedVersion);

  std::vector<std::byte> magic = valid;
  magic[0] = std::byte{'X'};
  CHECK(DecodeError(magic) == PrefabError::kBadMagic);
}

TEST_CASE("Checksums catch corruption; bulk only when asked") {
  const std::vector<std::byte> valid = Encode(testing::MakeSamplePrefab());
  const std::vector<Entry> toc = Toc(valid);

  std::vector<std::byte> header = valid;
  header[kHeaderSize + 1] ^= std::byte{0x01};  // TOC
  CHECK(DecodeError(header) == PrefabError::kChecksumMismatch);

  const Entry& nodes = toc[1];
  std::vector<std::byte> metadata = valid;
  metadata[nodes.offset + 6] ^= std::byte{0x01};
  CHECK(DecodeError(metadata) == PrefabError::kChecksumMismatch);

  const auto bulk = std::ranges::find(toc, kBulk, &Entry::fourcc);
  REQUIRE(bulk != toc.end());
  std::vector<std::byte> texel = valid;
  texel[bulk->offset + bulk->size - 1] ^= std::byte{0x01};
  CHECK(Decode(texel).ok());
  auto verified = Decode(texel, /*verify_bulk=*/true);
  REQUIRE_FALSE(verified.ok());
  CHECK(verified.error() == PrefabError::kChecksumMismatch);
}

TEST_CASE("An index past the vertex count in the bulk data is rejected") {
  const Prefab sample = testing::MakeSamplePrefab();
  std::vector<std::byte> file = Encode(sample);
  const std::vector<Entry> toc = Toc(file);
  const auto bulk = std::ranges::find(toc, kBulk, &Entry::fourcc);
  REQUIRE(bulk != toc.end());
  // Indices follow the vertices at the next aligned offset of the bulk.
  const size_t vertex_bytes = sample.meshes[0].vertices.size_bytes();
  const size_t index_offset = bulk->offset + (vertex_bytes + 15) / 16 * 16;
  REQUIRE(GetU32(file, index_offset) == sample.meshes[0].indices[0]);
  PutU32(file, index_offset, 1000000);
  CHECK(DecodeError(file) == PrefabError::kCorrupt);
}

TEST_CASE("Truncated files never decode") {
  const std::vector<std::byte> valid = Encode(testing::MakeSamplePrefab());
  for (size_t size = 0; size < valid.size(); ++size) {
    std::vector<std::byte> cut(valid.begin(),
                               valid.begin() + static_cast<ptrdiff_t>(size));
    auto decoded = Decode(cut);
    CHECK_FALSE(decoded.ok());
  }
}

TEST_CASE("Byte flips never crash the decoder, even with valid checksums") {
  const std::vector<std::byte> valid = Encode(testing::MakeSamplePrefab());
  int accepted = 0;
  for (size_t at = 0; at < valid.size(); ++at) {
    for (const std::byte mask :
         {std::byte{0x01}, std::byte{0x80}, std::byte{0xFF}}) {
      std::vector<std::byte> flipped = valid;
      flipped[at] ^= mask;
      // Plain flip: caught by a checksum, or harmless (bulk bytes, padding).
      auto plain = Decode(flipped);
      // Fixed checksums: reaches the parsers and the validation.
      FixChecksums(flipped);
      auto fixed = Decode(flipped);
      if (fixed.ok()) {
        ++accepted;
        CHECK_FALSE(ValidatePrefab(fixed.value()).has_value());
      }
      if (plain.ok()) {
        CHECK_FALSE(ValidatePrefab(plain.value()).has_value());
      }
    }
  }
  MESSAGE("flips decoded as valid prefabs (bulk bytes, float values): ",
          accepted);
}

namespace {

// The sample prefab with its paths inside `scratch` (absolute), so it loads
// back unchanged from a file there.
Prefab SampleIn(const testing::ScratchDirectory& scratch) {
  Prefab prefab = testing::MakeSamplePrefab();
  prefab.children[0].path = scratch.Child("parts/sight.pbo");
  prefab.edit.source_model = scratch.Child("sources/glock.gltf");
  prefab.edit.texture_sources = {{scratch.Child("sources/albedo.png")},
                                 {scratch.Child("sources/normal.png")}};
  return prefab;
}

}  // namespace

TEST_CASE("LoadPrefab maps the file instead of copying it") {
  const testing::ScratchDirectory scratch("pbo_map");
  const std::string path = scratch.Child("mapped.pbo");
  REQUIRE(SavePrefab(SampleIn(scratch), path).ok());
  const auto bytes = platform::ReadFile(path, kMaxPrefabFileSize);
  REQUIRE(bytes.ok());

  auto loaded = LoadPrefab(path);
  REQUIRE(loaded.ok());
  const BulkStorage& storage = loaded.value().storage;
  REQUIRE(storage.block_count() == 1);
  CHECK(storage.blocks()[0].kind == BlockKind::kMapped);
  CHECK(storage.byte_size(BlockKind::kMapped) == bytes.value().size());
  CHECK(storage.byte_size(BlockKind::kHeap) == 0);
  CheckSamePrefab(SampleIn(scratch), loaded.value());
}

TEST_CASE("Saving over a loaded prefab's file keeps the loaded prefab intact") {
  const testing::ScratchDirectory scratch("pbo_resave");
  const std::string path = scratch.Child("doc.pbo");
  const Prefab original = SampleIn(scratch);
  REQUIRE(SavePrefab(original, path).ok());
  auto loaded = LoadPrefab(path);
  REQUIRE(loaded.ok());

  // The editor saves an edited copy that still views the mapped file over
  // that same file, then keeps the old version for undo.
  Prefab edited = loaded.value();
  edited.materials[0].name = "brass";
  edited.textures.pop_back();
  edited.materials[0].textures[2] = -1;
  edited.edit.texture_sources.pop_back();
  REQUIRE(SavePrefab(edited, path).ok());

  CheckSamePrefab(original, loaded.value());
  auto reloaded = LoadPrefab(path);
  REQUIRE(reloaded.ok());
  CHECK(reloaded.value().materials[0].name == "brass");
  CHECK(reloaded.value().textures.size() == 1);
  CheckSamePrefab(edited, reloaded.value());

  // Saving again while both versions are mapped works too.
  REQUIRE(SavePrefab(loaded.value(), path).ok());
  CheckSamePrefab(edited, reloaded.value());
  auto third = LoadPrefab(path);
  REQUIRE(third.ok());
  CheckSamePrefab(original, third.value());
}

TEST_CASE("A loaded prefab outlives its file being deleted or replaced") {
  const testing::ScratchDirectory scratch("pbo_gone");
  const std::string path = scratch.Child("gone.pbo");
  REQUIRE(SavePrefab(SampleIn(scratch), path).ok());
  {
    auto loaded = LoadPrefab(path);
    REQUIRE(loaded.ok());
    // What a cooker regenerating the file does: write elsewhere, rename over.
    const std::vector<std::byte> other = Encode(Prefab{});
    REQUIRE(platform::WriteFileAtomic(path, other).ok());
    CheckSamePrefab(SampleIn(scratch), loaded.value());
    CHECK(platform::RemovePath(path));
    CHECK_FALSE(platform::PathExists(path));
    CheckSamePrefab(SampleIn(scratch), loaded.value());
  }
  // Writing a new file at the path works once the mapping is gone.
  REQUIRE(SavePrefab(SampleIn(scratch), path).ok());
  CHECK(LoadPrefab(path).ok());
}

#ifdef _WIN32
TEST_CASE("A mapped prefab file cannot be rewritten in place") {
  const testing::ScratchDirectory scratch("pbo_inplace");
  const std::string path = scratch.Child("locked.pbo");
  REQUIRE(SavePrefab(SampleIn(scratch), path).ok());
  auto loaded = LoadPrefab(path);
  REQUIRE(loaded.ok());
  // Opening for writing (which would truncate) is refused; reading is fine.
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
  CHECK(stream == nullptr);
  if (stream != nullptr) {
    SDL_CloseIO(stream);
  }
  CHECK(platform::ReadFile(path, kMaxPrefabFileSize).ok());
  CheckSamePrefab(SampleIn(scratch), loaded.value());
}
#endif

TEST_CASE("MappedFile maps whole files and reports errors") {
  const testing::ScratchDirectory scratch("mapped_file");
  const std::vector<std::byte> bytes = testing::PatternBytes(100000, 3);
  const std::string path = scratch.Child("data.bin");
  REQUIRE(platform::WriteFileAtomic(path, bytes).ok());
  auto mapped = platform::MappedFile::Open(path, bytes.size());
  REQUIRE(mapped.ok());
  CHECK(std::ranges::equal(mapped.value()->bytes(), bytes));

  auto too_large = platform::MappedFile::Open(path, bytes.size() - 1);
  REQUIRE_FALSE(too_large.ok());
  CHECK(too_large.error() == platform::FileError::kTooLarge);
  auto missing = platform::MappedFile::Open(scratch.Child("no.bin"), 1000);
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error() == platform::FileError::kNotFound);

  const std::string empty = scratch.Child("empty.bin");
  REQUIRE(platform::WriteFileAtomic(empty, {}).ok());
  auto empty_mapped = platform::MappedFile::Open(empty, 1000);
  REQUIRE(empty_mapped.ok());
  CHECK(empty_mapped.value()->bytes().empty());
}

TEST_CASE("Large prefab timing") {
  // ~40 MB: 600k vertices (56 B each = 33.6 MB), 1.8M indices (7.2 MB) and a
  // 2048^2 BC7 texture with mips (5.6 MB).
  const Prefab large = MakeLargePrefab(600000);

  auto start = std::chrono::steady_clock::now();
  std::vector<std::byte> file = Encode(large);
  const double encode_ms = MillisecondsSince(start);
  const double megabytes = static_cast<double>(file.size()) / (1024.0 * 1024.0);

  start = std::chrono::steady_clock::now();
  const uint32_t crc = scene::Crc32(file);
  const double crc_ms = MillisecondsSince(start);

  std::vector<std::byte> copy = file;
  start = std::chrono::steady_clock::now();
  auto decoded = Decode(std::move(file));
  const double decode_ms = MillisecondsSince(start);
  REQUIRE(decoded.ok());

  start = std::chrono::steady_clock::now();
  auto verified = Decode(std::move(copy), /*verify_bulk=*/true);
  const double verified_ms = MillisecondsSince(start);
  REQUIRE(verified.ok());
  CHECK(verified.value().meshes[0].vertices.size() == 600000);

  MESSAGE("large prefab ", megabytes, " MB: encode ", encode_ms, " ms, decode ",
          decode_ms, " ms, decode with bulk CRC ", verified_ms,
          " ms, CRC-32 of the whole file ", crc_ms, " ms (crc ", crc, ")");
}

}  // namespace blue::prefab
