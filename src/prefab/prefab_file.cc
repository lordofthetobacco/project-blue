#include "prefab/prefab_file.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "core/log.h"
#include "core/path.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"

namespace blue::prefab {
namespace {

// Bulk arrays are copied to and from disk as raw host memory.
static_assert(std::endian::native == std::endian::little,
              "the .pbo bulk layout is little-endian host memory");
static_assert(std::is_trivially_copyable_v<asset::Vertex>);
static_assert(sizeof(asset::Vertex) % sizeof(float) == 0);
static_assert(sizeof(asset::Index) == sizeof(uint32_t));

using scene::BinaryReader;
using scene::BinaryWriter;
using scene::MakeFourcc;
using scene::ReadStatus;

using Status = core::Result<void, PrefabError>;
using BytesResult = core::Result<std::vector<std::byte>, PrefabError>;
using PrefabResult = core::Result<Prefab, PrefabError>;

constexpr std::array<char, 8> kMagic = {'B', 'L', 'U', 'E',
                                        'P', 'B', 'O', '\0'};
// Magic, version, file size, chunk count, vertex stride, flags, CRC.
constexpr size_t kHeaderSize = kMagic.size() + 6 * sizeof(uint32_t);
constexpr size_t kHeaderCrcOffset = kHeaderSize - sizeof(uint32_t);
// fourcc, offset, size, CRC.
constexpr size_t kTocEntrySize = 4 * sizeof(uint32_t);
constexpr uint32_t kMaxChunkCount = 4096;

constexpr uint32_t kInfoChunk = MakeFourcc('I', 'N', 'F', 'O');
constexpr uint32_t kNodeChunk = MakeFourcc('N', 'O', 'D', 'E');
constexpr uint32_t kMeshChunk = MakeFourcc('M', 'E', 'S', 'H');
constexpr uint32_t kMaterialChunk = MakeFourcc('M', 'A', 'T', 'L');
constexpr uint32_t kTextureChunk = MakeFourcc('T', 'E', 'X', 'R');
constexpr uint32_t kClipChunk = MakeFourcc('C', 'L', 'I', 'P');
constexpr uint32_t kSocketChunk = MakeFourcc('S', 'O', 'C', 'K');
constexpr uint32_t kLightChunk = MakeFourcc('L', 'G', 'H', 'T');
constexpr uint32_t kChildChunk = MakeFourcc('C', 'H', 'L', 'D');
constexpr uint32_t kPhysicsChunk = MakeFourcc('P', 'H', 'Y', 'S');
constexpr uint32_t kEditChunk = MakeFourcc('E', 'D', 'I', 'T');
constexpr uint32_t kBulkChunk = MakeFourcc('B', 'U', 'L', 'K');

constexpr uint32_t kDoubleSidedFlag = 1U << 0;
constexpr uint32_t kLoopFlag = 1U << 0;
constexpr uint32_t kDynamicFlag = 1U << 0;

// Smallest encodings of the variable-size records, used to reject hostile
// counts before they drive an allocation. A string is at least its u32
// length; a transform is 10 floats.
constexpr size_t kStringBytes = sizeof(uint32_t);
constexpr size_t kTransformBytes = 10 * sizeof(float);
constexpr size_t kMinNodeBytes = kStringBytes + 4 + kTransformBytes;
constexpr size_t kMinMeshBytes = kStringBytes + 4 + 6 * 4 + 4 * 4 + 4;
constexpr size_t kSubmeshBytes = 3 * sizeof(uint32_t);
constexpr size_t kMaterialBytesAfterName =
    (4 + 1 + 1 + 3 + 1 + 1 + 1 + 2 + 1 + 1 + 1) * 4 + 2 * kTextureSlotCount * 4;
constexpr size_t kMinTextureBytes = kStringBytes + 4 * 4;
constexpr size_t kBulkRefBytes = 2 * sizeof(uint32_t);
constexpr size_t kMinClipBytes = kStringBytes + 4 * 4;
constexpr size_t kMinChannelBytes = 5 * sizeof(uint32_t);
constexpr size_t kMinEventBytes = kStringBytes + 4;
constexpr size_t kMinSocketBytes = kStringBytes + 4 + kTransformBytes;
constexpr size_t kMinLightBytes = kStringBytes + 4 + 8 * 4;
constexpr size_t kMinChildBytes = kStringBytes + 4 + kTransformBytes;

// Returns `value` rounded up to a multiple of kBulkAlignment.
constexpr size_t AlignUp(size_t value) {
  return (value + kBulkAlignment - 1) / kBulkAlignment * kBulkAlignment;
}

// ---------------------------------------------------------------------------
// Primitive encoding.

void WriteI32(BinaryWriter& writer, int value) {
  writer.WriteU32(static_cast<uint32_t>(value));
}

int ReadI32(BinaryReader& reader) {
  return static_cast<int32_t>(reader.ReadU32());
}

void WriteVec2(BinaryWriter& writer, const glm::vec2& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
}

void WriteVec3(BinaryWriter& writer, const glm::vec3& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
  writer.WriteF32(v.z);
}

void WriteVec4(BinaryWriter& writer, const glm::vec4& v) {
  writer.WriteF32(v.x);
  writer.WriteF32(v.y);
  writer.WriteF32(v.z);
  writer.WriteF32(v.w);
}

void WriteQuat(BinaryWriter& writer, const glm::quat& q) {
  writer.WriteF32(q.x);
  writer.WriteF32(q.y);
  writer.WriteF32(q.z);
  writer.WriteF32(q.w);
}

void WriteTransform(BinaryWriter& writer, const animation::Transform& t) {
  WriteVec3(writer, t.translation);
  WriteQuat(writer, t.rotation);
  WriteVec3(writer, t.scale);
}

void WriteBounds(BinaryWriter& writer, const asset::Bounds& bounds) {
  WriteVec3(writer, bounds.min);
  WriteVec3(writer, bounds.max);
}

glm::vec2 ReadVec2(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  return {x, y};
}

glm::vec3 ReadVec3(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  return {x, y, z};
}

glm::vec4 ReadVec4(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  return {x, y, z, w};
}

glm::quat ReadQuat(BinaryReader& reader) {
  const float x = reader.ReadF32();
  const float y = reader.ReadF32();
  const float z = reader.ReadF32();
  const float w = reader.ReadF32();
  return {w, x, y, z};
}

animation::Transform ReadTransform(BinaryReader& reader) {
  animation::Transform t;
  t.translation = ReadVec3(reader);
  t.rotation = ReadQuat(reader);
  t.scale = ReadVec3(reader);
  return t;
}

asset::Bounds ReadBounds(BinaryReader& reader) {
  asset::Bounds bounds;
  bounds.min = ReadVec3(reader);
  bounds.max = ReadVec3(reader);
  return bounds;
}

// ---------------------------------------------------------------------------
// Paths.

// Returns the on-disk form of `path`: relative to `base_dir` where possible.
// Empty paths and an empty `base_dir` leave the path unchanged.
std::string StoredPath(const std::string& path, std::string_view base_dir) {
  if (path.empty() || base_dir.empty()) {
    return path;
  }
  const std::string absolute = platform::AbsolutePath(path);
  return core::RelativePath(absolute, platform::AbsolutePath(base_dir))
      .value_or(absolute);
}

// Returns the in-memory form of a `stored` path: the inverse of StoredPath().
std::string ResolvedPath(const std::string& stored, std::string_view base_dir) {
  if (stored.empty() || base_dir.empty()) {
    return stored;
  }
  return core::JoinPath(base_dir, stored);
}

// ---------------------------------------------------------------------------
// Encoding.

// Plans the 'BULK' chunk: assigns each array an aligned offset relative to the
// chunk start and remembers where to copy it from. Nothing is copied until
// the file is assembled.
class BulkLayout {
 public:
  struct Piece {
    std::span<const std::byte> source;
    size_t offset = 0;
  };

  // Writes the { offset, size } reference of `bytes` to `writer` and plans
  // its copy. Empty arrays get offset 0 and take no space.
  void WriteRef(BinaryWriter& writer, std::span<const std::byte> bytes) {
    size_t offset = 0;
    if (!bytes.empty()) {
      offset = AlignUp(size_);
      pieces_.push_back({bytes, offset});
      size_ = offset + bytes.size();
    }
    writer.WriteU32(static_cast<uint32_t>(offset));
    writer.WriteU32(static_cast<uint32_t>(bytes.size()));
  }

  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] const std::vector<Piece>& pieces() const { return pieces_; }

 private:
  std::vector<Piece> pieces_;
  size_t size_ = 0;
};

std::vector<std::byte> EncodeInfo(const Prefab& prefab) {
  BinaryWriter writer;
  WriteBounds(writer, prefab.bounds);
  return writer.bytes();
}

std::vector<std::byte> EncodeNodes(const Prefab& prefab) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.hierarchy.nodes.size()));
  for (const animation::Node& node : prefab.hierarchy.nodes) {
    writer.WriteString(node.name);
    WriteI32(writer, node.parent);
    WriteTransform(writer, node.rest);
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeMeshes(const Prefab& prefab, BulkLayout& bulk) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.meshes.size()));
  for (const Mesh& mesh : prefab.meshes) {
    writer.WriteString(mesh.name);
    WriteI32(writer, mesh.node);
    WriteBounds(writer, mesh.bounds);
    bulk.WriteRef(writer, std::as_bytes(mesh.vertices));
    bulk.WriteRef(writer, std::as_bytes(mesh.indices));
    writer.WriteU32(static_cast<uint32_t>(mesh.submeshes.size()));
    for (const asset::Submesh& submesh : mesh.submeshes) {
      writer.WriteU32(submesh.first_index);
      writer.WriteU32(submesh.index_count);
      WriteI32(writer, submesh.material);
    }
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeMaterials(const Prefab& prefab) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.materials.size()));
  for (const Material& material : prefab.materials) {
    writer.WriteString(material.name);
    WriteVec4(writer, material.base_color);
    writer.WriteF32(material.metallic);
    writer.WriteF32(material.roughness);
    WriteVec3(writer, material.emissive);
    writer.WriteF32(material.emissive_strength);
    writer.WriteF32(material.normal_scale);
    writer.WriteF32(material.occlusion_strength);
    WriteVec2(writer, material.uv_scale);
    writer.WriteU32(static_cast<uint32_t>(material.alpha_mode));
    writer.WriteF32(material.alpha_cutoff);
    writer.WriteU32(material.double_sided ? kDoubleSidedFlag : 0U);
    for (const int texture : material.textures) {
      WriteI32(writer, texture);
    }
    for (const int uv_set : material.uv_sets) {
      WriteI32(writer, uv_set);
    }
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeTextures(const Prefab& prefab, BulkLayout& bulk) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.textures.size()));
  for (const Texture& texture : prefab.textures) {
    writer.WriteString(texture.name);
    writer.WriteU32(static_cast<uint32_t>(texture.format));
    WriteI32(writer, texture.width);
    WriteI32(writer, texture.height);
    writer.WriteU32(static_cast<uint32_t>(texture.mips.size()));
    for (const TextureMip& mip : texture.mips) {
      bulk.WriteRef(writer, mip.data);
    }
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeClips(const Prefab& prefab) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.clips.size()));
  for (const animation::Clip& clip : prefab.clips) {
    writer.WriteString(clip.name);
    writer.WriteF32(clip.duration);
    writer.WriteU32(clip.loop ? kLoopFlag : 0U);
    writer.WriteU32(static_cast<uint32_t>(clip.channels.size()));
    for (const animation::Channel& channel : clip.channels) {
      WriteI32(writer, channel.node);
      writer.WriteU32(static_cast<uint32_t>(channel.path));
      writer.WriteU32(static_cast<uint32_t>(channel.interpolation));
      writer.WriteU32(static_cast<uint32_t>(channel.times.size()));
      writer.WriteU32(static_cast<uint32_t>(channel.values.size()));
      for (const float time : channel.times) {
        writer.WriteF32(time);
      }
      for (const float value : channel.values) {
        writer.WriteF32(value);
      }
    }
    writer.WriteU32(static_cast<uint32_t>(clip.events.size()));
    for (const animation::ClipEvent& event : clip.events) {
      writer.WriteString(event.name);
      writer.WriteF32(event.time);
    }
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeSockets(const Prefab& prefab) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.sockets.size()));
  for (const Socket& socket : prefab.sockets) {
    writer.WriteString(socket.name);
    WriteI32(writer, socket.node);
    WriteTransform(writer, socket.local);
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeLights(const Prefab& prefab) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.lights.size()));
  for (const PointLight& light : prefab.lights) {
    writer.WriteString(light.name);
    WriteI32(writer, light.node);
    WriteVec3(writer, light.position);
    WriteVec3(writer, light.color);
    writer.WriteF32(light.intensity);
    writer.WriteF32(light.range);
  }
  return writer.bytes();
}

std::vector<std::byte> EncodeChildren(const Prefab& prefab,
                                      std::string_view base_dir) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(prefab.children.size()));
  for (const ChildPrefab& child : prefab.children) {
    writer.WriteString(StoredPath(child.path, base_dir));
    WriteI32(writer, child.node);
    WriteTransform(writer, child.local);
  }
  return writer.bytes();
}

std::vector<std::byte> EncodePhysics(const Physics& physics, BulkLayout& bulk) {
  BinaryWriter writer;
  writer.WriteU32(static_cast<uint32_t>(physics.kind));
  WriteVec3(writer, physics.center);
  WriteQuat(writer, physics.rotation);
  WriteVec3(writer, physics.half_extents);
  writer.WriteF32(physics.radius);
  writer.WriteF32(physics.half_height);
  writer.WriteU32(physics.dynamic ? kDynamicFlag : 0U);
  writer.WriteF32(physics.mass);
  writer.WriteF32(physics.friction);
  writer.WriteF32(physics.restitution);
  bulk.WriteRef(writer, physics.cooked_shape);
  return writer.bytes();
}

std::vector<std::byte> EncodeEdit(const EditInfo& edit,
                                  std::string_view base_dir) {
  BinaryWriter writer;
  writer.WriteString(StoredPath(edit.source_model, base_dir));
  writer.WriteString(edit.import_settings);
  writer.WriteU32(static_cast<uint32_t>(edit.texture_sources.size()));
  for (const std::vector<std::string>& sources : edit.texture_sources) {
    writer.WriteU32(static_cast<uint32_t>(sources.size()));
    for (const std::string& source : sources) {
      writer.WriteString(StoredPath(source, base_dir));
    }
  }
  return writer.bytes();
}

// Returns true if every path of `prefab` still fits kMaxStringLength once
// stored relative to `base_dir` (an absolute fallback can be longer).
bool StoredPathsFit(const Prefab& prefab, std::string_view base_dir) {
  const auto fits = [base_dir](const std::string& path) {
    return StoredPath(path, base_dir).size() <= kMaxStringLength;
  };
  if (!fits(prefab.edit.source_model)) {
    return false;
  }
  for (const ChildPrefab& child : prefab.children) {
    if (!fits(child.path)) {
      return false;
    }
  }
  for (const std::vector<std::string>& sources : prefab.edit.texture_sources) {
    if (!std::ranges::all_of(sources, fits)) {
      return false;
    }
  }
  return true;
}

// Writes `value` little-endian at `offset` of `file`.
void PutU32(std::vector<std::byte>& file, size_t offset, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) {
    file[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
  }
}

// ---------------------------------------------------------------------------
// Decoding.

Status ToStatus(ReadStatus status) {
  switch (status) {
    case ReadStatus::kOk:
      return Status::Ok();
    case ReadStatus::kTruncated:
    case ReadStatus::kLimitExceeded:
      break;
  }
  // Every chunk declares its size, so running past its end is malformed
  // content rather than a short file.
  return Status::Error(PrefabError::kCorrupt);
}

// Returns true if `count` records of at least `min_bytes` each can still fit
// in `reader`.
bool CountFits(const BinaryReader& reader, uint32_t count, size_t min_bytes) {
  return count <= reader.remaining() / min_bytes;
}

// Reads chunk bodies whose records may reference ranges of the 'BULK' chunk.
// Keeps the first failure; callers check ok() after reading a record.
class ChunkDecoder {
 public:
  ChunkDecoder(std::span<const std::byte> body, std::span<const std::byte> bulk)
      : reader_(body), bulk_(bulk) {}

  BinaryReader& reader() { return reader_; }

  // Reads a { offset, size } reference and returns the bytes it names, or an
  // empty span and a failure if it is misaligned, out of range or not a
  // multiple of `element_size`.
  std::span<const std::byte> ReadBulk(size_t element_size) {
    const uint32_t offset = reader_.ReadU32();
    const uint32_t size = reader_.ReadU32();
    if (reader_.status() != ReadStatus::kOk || size == 0) {
      return {};
    }
    if (offset % kBulkAlignment != 0 || offset > bulk_.size() ||
        size > bulk_.size() - offset || size % element_size != 0) {
      bad_ref_ = true;
      return {};
    }
    return bulk_.subspan(offset, size);
  }

  template <typename T>
  std::span<const T> ReadBulkArray() {
    const std::span<const std::byte> bytes = ReadBulk(sizeof(T));
    return {reinterpret_cast<const T*>(bytes.data()), bytes.size() / sizeof(T)};
  }

  std::string ReadString(size_t max_length = kMaxStringLength) {
    return reader_.ReadString(static_cast<uint32_t>(max_length));
  }

  // Returns the first failure so far, if any.
  [[nodiscard]] Status status() const {
    if (bad_ref_) {
      return Status::Error(PrefabError::kCorrupt);
    }
    return ToStatus(reader_.status());
  }

  // Returns the status, also failing if the body was not consumed exactly.
  [[nodiscard]] Status Finish() const {
    if (Status current = status(); !current.ok()) {
      return current;
    }
    if (reader_.remaining() != 0) {
      return Status::Error(PrefabError::kCorrupt);
    }
    return Status::Ok();
  }

 private:
  BinaryReader reader_;
  std::span<const std::byte> bulk_;
  bool bad_ref_ = false;
};

// Reads a record count and checks that `count` records of `min_bytes` fit.
std::optional<uint32_t> ReadCount(ChunkDecoder& decoder, size_t min_bytes) {
  const uint32_t count = decoder.reader().ReadU32();
  if (!decoder.status().ok() ||
      !CountFits(decoder.reader(), count, min_bytes)) {
    return std::nullopt;
  }
  return count;
}

Status Corrupt() { return Status::Error(PrefabError::kCorrupt); }

Status DecodeInfo(ChunkDecoder decoder, Prefab& prefab) {
  prefab.bounds = ReadBounds(decoder.reader());
  return decoder.Finish();
}

Status DecodeNodes(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinNodeBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.hierarchy.nodes.resize(*count);
  for (animation::Node& node : prefab.hierarchy.nodes) {
    node.name = decoder.ReadString();
    node.parent = ReadI32(decoder.reader());
    node.rest = ReadTransform(decoder.reader());
  }
  return decoder.Finish();
}

Status DecodeMeshes(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinMeshBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.meshes.resize(*count);
  for (Mesh& mesh : prefab.meshes) {
    mesh.name = decoder.ReadString();
    mesh.node = ReadI32(decoder.reader());
    mesh.bounds = ReadBounds(decoder.reader());
    mesh.vertices = decoder.ReadBulkArray<asset::Vertex>();
    mesh.indices = decoder.ReadBulkArray<asset::Index>();
    const std::optional<uint32_t> submeshes = ReadCount(decoder, kSubmeshBytes);
    if (!submeshes) {
      return Corrupt();
    }
    mesh.submeshes.resize(*submeshes);
    for (asset::Submesh& submesh : mesh.submeshes) {
      submesh.first_index = decoder.reader().ReadU32();
      submesh.index_count = decoder.reader().ReadU32();
      submesh.material = ReadI32(decoder.reader());
    }
    if (Status status = decoder.status(); !status.ok()) {
      return status;
    }
  }
  return decoder.Finish();
}

Status DecodeMaterials(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count =
      ReadCount(decoder, kStringBytes + kMaterialBytesAfterName);
  if (!count) {
    return Corrupt();
  }
  prefab.materials.resize(*count);
  for (Material& material : prefab.materials) {
    BinaryReader& reader = decoder.reader();
    material.name = decoder.ReadString();
    material.base_color = ReadVec4(reader);
    material.metallic = reader.ReadF32();
    material.roughness = reader.ReadF32();
    material.emissive = ReadVec3(reader);
    material.emissive_strength = reader.ReadF32();
    material.normal_scale = reader.ReadF32();
    material.occlusion_strength = reader.ReadF32();
    material.uv_scale = ReadVec2(reader);
    const uint32_t alpha_mode = reader.ReadU32();
    material.alpha_cutoff = reader.ReadF32();
    const uint32_t flags = reader.ReadU32();
    for (int& texture : material.textures) {
      texture = ReadI32(reader);
    }
    for (int& uv_set : material.uv_sets) {
      uv_set = ReadI32(reader);
    }
    if (alpha_mode > static_cast<uint32_t>(material::AlphaMode::kMask) ||
        (flags & ~kDoubleSidedFlag) != 0) {
      return Corrupt();
    }
    material.alpha_mode = static_cast<material::AlphaMode>(alpha_mode);
    material.double_sided = (flags & kDoubleSidedFlag) != 0;
  }
  return decoder.Finish();
}

Status DecodeTextures(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinTextureBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.textures.resize(*count);
  for (Texture& texture : prefab.textures) {
    texture.name = decoder.ReadString();
    const uint32_t format = decoder.reader().ReadU32();
    texture.width = ReadI32(decoder.reader());
    texture.height = ReadI32(decoder.reader());
    const std::optional<uint32_t> mips = ReadCount(decoder, kBulkRefBytes);
    if (!mips || format > static_cast<uint32_t>(TextureFormat::kRgba8Unorm) ||
        texture.width < 1 || texture.height < 1) {
      return Corrupt();
    }
    texture.format = static_cast<TextureFormat>(format);
    texture.mips.resize(*mips);
    for (size_t level = 0; level < texture.mips.size(); ++level) {
      TextureMip& mip = texture.mips[level];
      mip.width = std::max(1, texture.width >> std::min<size_t>(level, 31));
      mip.height = std::max(1, texture.height >> std::min<size_t>(level, 31));
      mip.data = decoder.ReadBulk(1);
    }
  }
  return decoder.Finish();
}

// Reads `count` floats into `values`.
void ReadFloats(BinaryReader& reader, uint32_t count,
                std::vector<float>& values) {
  values.resize(count);
  for (float& value : values) {
    value = reader.ReadF32();
  }
}

Status DecodeChannel(ChunkDecoder& decoder, animation::Channel& channel) {
  BinaryReader& reader = decoder.reader();
  channel.node = ReadI32(reader);
  const uint32_t path = reader.ReadU32();
  const uint32_t interpolation = reader.ReadU32();
  const uint32_t key_count = reader.ReadU32();
  const uint32_t value_count = reader.ReadU32();
  if (Status status = decoder.status(); !status.ok()) {
    return status;
  }
  const uint64_t float_count = uint64_t{key_count} + uint64_t{value_count};
  if (path > static_cast<uint32_t>(animation::TargetPath::kScale) ||
      interpolation >
          static_cast<uint32_t>(animation::Interpolation::kCubicSpline) ||
      float_count > reader.remaining() / sizeof(float)) {
    return Corrupt();
  }
  channel.path = static_cast<animation::TargetPath>(path);
  channel.interpolation = static_cast<animation::Interpolation>(interpolation);
  ReadFloats(reader, key_count, channel.times);
  ReadFloats(reader, value_count, channel.values);
  return decoder.status();
}

Status DecodeClips(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinClipBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.clips.resize(*count);
  for (animation::Clip& clip : prefab.clips) {
    clip.name = decoder.ReadString();
    clip.duration = decoder.reader().ReadF32();
    const uint32_t flags = decoder.reader().ReadU32();
    if ((flags & ~kLoopFlag) != 0) {
      return Corrupt();
    }
    clip.loop = (flags & kLoopFlag) != 0;
    const std::optional<uint32_t> channels =
        ReadCount(decoder, kMinChannelBytes);
    if (!channels) {
      return Corrupt();
    }
    clip.channels.resize(*channels);
    for (animation::Channel& channel : clip.channels) {
      if (Status status = DecodeChannel(decoder, channel); !status.ok()) {
        return status;
      }
    }
    const std::optional<uint32_t> events = ReadCount(decoder, kMinEventBytes);
    if (!events) {
      return Corrupt();
    }
    clip.events.resize(*events);
    for (animation::ClipEvent& event : clip.events) {
      event.name = decoder.ReadString();
      event.time = decoder.reader().ReadF32();
    }
  }
  return decoder.Finish();
}

Status DecodeSockets(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinSocketBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.sockets.resize(*count);
  for (Socket& socket : prefab.sockets) {
    socket.name = decoder.ReadString();
    socket.node = ReadI32(decoder.reader());
    socket.local = ReadTransform(decoder.reader());
  }
  return decoder.Finish();
}

Status DecodeLights(ChunkDecoder decoder, Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinLightBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.lights.resize(*count);
  for (PointLight& light : prefab.lights) {
    light.name = decoder.ReadString();
    light.node = ReadI32(decoder.reader());
    light.position = ReadVec3(decoder.reader());
    light.color = ReadVec3(decoder.reader());
    light.intensity = decoder.reader().ReadF32();
    light.range = decoder.reader().ReadF32();
  }
  return decoder.Finish();
}

Status DecodeChildren(ChunkDecoder decoder, std::string_view base_dir,
                      Prefab& prefab) {
  const std::optional<uint32_t> count = ReadCount(decoder, kMinChildBytes);
  if (!count) {
    return Corrupt();
  }
  prefab.children.resize(*count);
  for (ChildPrefab& child : prefab.children) {
    child.path = ResolvedPath(decoder.ReadString(), base_dir);
    child.node = ReadI32(decoder.reader());
    child.local = ReadTransform(decoder.reader());
  }
  return decoder.Finish();
}

Status DecodePhysics(ChunkDecoder decoder, Prefab& prefab) {
  BinaryReader& reader = decoder.reader();
  Physics& physics = prefab.physics;
  const uint32_t kind = reader.ReadU32();
  physics.center = ReadVec3(reader);
  physics.rotation = ReadQuat(reader);
  physics.half_extents = ReadVec3(reader);
  physics.radius = reader.ReadF32();
  physics.half_height = reader.ReadF32();
  const uint32_t flags = reader.ReadU32();
  physics.mass = reader.ReadF32();
  physics.friction = reader.ReadF32();
  physics.restitution = reader.ReadF32();
  physics.cooked_shape = decoder.ReadBulk(1);
  if (kind > static_cast<uint32_t>(ColliderKind::kTriangleMesh) ||
      (flags & ~kDynamicFlag) != 0) {
    return Corrupt();
  }
  physics.kind = static_cast<ColliderKind>(kind);
  physics.dynamic = (flags & kDynamicFlag) != 0;
  return decoder.Finish();
}

Status DecodeEdit(ChunkDecoder decoder, std::string_view base_dir,
                  Prefab& prefab) {
  EditInfo& edit = prefab.edit;
  edit.source_model = ResolvedPath(decoder.ReadString(), base_dir);
  edit.import_settings = decoder.ReadString(kMaxSettingsLength);
  const std::optional<uint32_t> count = ReadCount(decoder, kStringBytes);
  if (!count) {
    return Corrupt();
  }
  edit.texture_sources.resize(*count);
  for (std::vector<std::string>& sources : edit.texture_sources) {
    const std::optional<uint32_t> paths = ReadCount(decoder, kStringBytes);
    if (!paths) {
      return Corrupt();
    }
    sources.resize(*paths);
    for (std::string& source : sources) {
      source = ResolvedPath(decoder.ReadString(), base_dir);
    }
  }
  return decoder.Finish();
}

struct TocEntry {
  uint32_t fourcc = 0;
  uint32_t offset = 0;
  uint32_t size = 0;
  uint32_t crc = 0;
};

// Validates the header and table of contents of `file` and returns the
// entries. Checks the header CRC, file size, vertex stride, and that every
// chunk is aligned, in bounds, after the TOC and not overlapping the next.
core::Result<std::vector<TocEntry>, PrefabError> ReadToc(
    std::span<const std::byte> file) {
  using TocResult = core::Result<std::vector<TocEntry>, PrefabError>;
  if (file.size() < kHeaderSize) {
    return TocResult::Error(PrefabError::kTruncated);
  }
  BinaryReader header(file);
  const std::span<const std::byte> magic = header.ReadBytes(kMagic.size());
  if (!std::equal(
          magic.begin(), magic.end(), kMagic.begin(), [](std::byte a, char b) {
            return std::to_integer<int>(a) == static_cast<unsigned char>(b);
          })) {
    return TocResult::Error(PrefabError::kBadMagic);
  }
  const uint32_t version = header.ReadU32();
  const uint32_t file_size = header.ReadU32();
  const uint32_t chunk_count = header.ReadU32();
  const uint32_t vertex_stride = header.ReadU32();
  const uint32_t flags = header.ReadU32();
  const uint32_t header_crc = header.ReadU32();
  if (version == 0) {
    return TocResult::Error(PrefabError::kCorrupt);
  }
  if (version > kPrefabFormatVersion) {
    return TocResult::Error(PrefabError::kUnsupportedVersion);
  }
  if (file_size > file.size()) {
    return TocResult::Error(PrefabError::kTruncated);
  }
  if (chunk_count > kMaxChunkCount ||
      kHeaderSize + size_t{chunk_count} * kTocEntrySize > file_size) {
    return TocResult::Error(PrefabError::kCorrupt);
  }
  const size_t toc_end = kHeaderSize + size_t{chunk_count} * kTocEntrySize;
  std::vector<std::byte> checked(file.begin(), file.begin() + kHeaderCrcOffset);
  checked.insert(checked.end(), file.begin() + kHeaderSize,
                 file.begin() + static_cast<ptrdiff_t>(toc_end));
  if (scene::Crc32(checked) != header_crc) {
    return TocResult::Error(PrefabError::kChecksumMismatch);
  }
  if (file_size != file.size() || flags != 0) {
    return TocResult::Error(PrefabError::kCorrupt);
  }
  if (vertex_stride != sizeof(asset::Vertex)) {
    return TocResult::Error(PrefabError::kUnsupportedVersion);
  }

  std::vector<TocEntry> entries(chunk_count);
  size_t previous_end = toc_end;
  for (TocEntry& entry : entries) {
    entry.fourcc = header.ReadU32();
    entry.offset = header.ReadU32();
    entry.size = header.ReadU32();
    entry.crc = header.ReadU32();
    if (entry.offset % kBulkAlignment != 0 || entry.offset < previous_end ||
        entry.offset > file_size || entry.size > file_size - entry.offset) {
      return TocResult::Error(PrefabError::kCorrupt);
    }
    previous_end = size_t{entry.offset} + entry.size;
  }
  return TocResult::Ok(std::move(entries));
}

// Decodes the metadata chunk `fourcc`. Unknown chunks are ignored.
Status DecodeChunk(uint32_t fourcc, std::span<const std::byte> body,
                   std::span<const std::byte> bulk, std::string_view base_dir,
                   Prefab& prefab) {
  const ChunkDecoder decoder(body, bulk);
  switch (fourcc) {
    case kInfoChunk:
      return DecodeInfo(decoder, prefab);
    case kNodeChunk:
      return DecodeNodes(decoder, prefab);
    case kMeshChunk:
      return DecodeMeshes(decoder, prefab);
    case kMaterialChunk:
      return DecodeMaterials(decoder, prefab);
    case kTextureChunk:
      return DecodeTextures(decoder, prefab);
    case kClipChunk:
      return DecodeClips(decoder, prefab);
    case kSocketChunk:
      return DecodeSockets(decoder, prefab);
    case kLightChunk:
      return DecodeLights(decoder, prefab);
    case kChildChunk:
      return DecodeChildren(decoder, base_dir, prefab);
    case kPhysicsChunk:
      return DecodePhysics(decoder, prefab);
    case kEditChunk:
      return DecodeEdit(decoder, base_dir, prefab);
    default:
      return Status::Ok();
  }
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

const char* ToString(PrefabError error) {
  switch (error) {
    case PrefabError::kCannotOpen:
      return "cannot open";
    case PrefabError::kWriteFailed:
      return "write failed";
    case PrefabError::kBadMagic:
      return "not a prefab file";
    case PrefabError::kUnsupportedVersion:
      return "unsupported version or vertex layout";
    case PrefabError::kTruncated:
      return "truncated";
    case PrefabError::kChecksumMismatch:
      return "checksum mismatch";
    case PrefabError::kCorrupt:
      return "corrupt";
    case PrefabError::kInvalidPrefab:
      return "invalid prefab";
  }
  return "unknown";
}

BytesResult EncodePrefab(const Prefab& prefab, std::string_view base_dir) {
  if (ValidatePrefab(prefab).has_value() || !StoredPathsFit(prefab, base_dir)) {
    return BytesResult::Error(PrefabError::kInvalidPrefab);
  }

  BulkLayout bulk;
  std::vector<std::pair<uint32_t, std::vector<std::byte>>> chunks;
  chunks.emplace_back(kInfoChunk, EncodeInfo(prefab));
  chunks.emplace_back(kNodeChunk, EncodeNodes(prefab));
  chunks.emplace_back(kMeshChunk, EncodeMeshes(prefab, bulk));
  chunks.emplace_back(kMaterialChunk, EncodeMaterials(prefab));
  chunks.emplace_back(kTextureChunk, EncodeTextures(prefab, bulk));
  chunks.emplace_back(kClipChunk, EncodeClips(prefab));
  chunks.emplace_back(kSocketChunk, EncodeSockets(prefab));
  chunks.emplace_back(kLightChunk, EncodeLights(prefab));
  chunks.emplace_back(kChildChunk, EncodeChildren(prefab, base_dir));
  chunks.emplace_back(kPhysicsChunk, EncodePhysics(prefab.physics, bulk));
  chunks.emplace_back(kEditChunk, EncodeEdit(prefab.edit, base_dir));
  const bool has_bulk = bulk.size() > 0;
  const size_t chunk_count = chunks.size() + (has_bulk ? 1 : 0);

  // Lay the chunks out after the TOC, each at an aligned offset.
  std::vector<TocEntry> toc;
  size_t end = AlignUp(kHeaderSize + chunk_count * kTocEntrySize);
  for (const auto& [fourcc, body] : chunks) {
    toc.push_back({.fourcc = fourcc,
                   .offset = static_cast<uint32_t>(end),
                   .size = static_cast<uint32_t>(body.size())});
    end = AlignUp(end + body.size());
  }
  size_t bulk_offset = 0;
  if (has_bulk) {
    bulk_offset = end;
    end += bulk.size();
  }
  if (end > kMaxPrefabFileSize) {
    return BytesResult::Error(PrefabError::kInvalidPrefab);
  }
  if (has_bulk) {
    toc.push_back({.fourcc = kBulkChunk,
                   .offset = static_cast<uint32_t>(bulk_offset),
                   .size = static_cast<uint32_t>(bulk.size())});
  }

  std::vector<std::byte> file(end);
  for (size_t i = 0; i < chunks.size(); ++i) {
    const std::vector<std::byte>& body = chunks[i].second;
    std::ranges::copy(body, file.begin() + toc[i].offset);
    toc[i].crc = scene::Crc32(body);
  }
  if (has_bulk) {
    for (const BulkLayout::Piece& piece : bulk.pieces()) {
      std::memcpy(file.data() + bulk_offset + piece.offset, piece.source.data(),
                  piece.source.size());
    }
    toc.back().crc = scene::Crc32(
        std::span<const std::byte>(file).subspan(bulk_offset, bulk.size()));
  }

  std::ranges::transform(kMagic, file.begin(),
                         [](char c) { return static_cast<std::byte>(c); });
  PutU32(file, 8, kPrefabFormatVersion);
  PutU32(file, 12, static_cast<uint32_t>(file.size()));
  PutU32(file, 16, static_cast<uint32_t>(toc.size()));
  PutU32(file, 20, static_cast<uint32_t>(sizeof(asset::Vertex)));
  PutU32(file, 24, 0);  // flags
  for (size_t i = 0; i < toc.size(); ++i) {
    const size_t entry = kHeaderSize + i * kTocEntrySize;
    PutU32(file, entry, toc[i].fourcc);
    PutU32(file, entry + 4, toc[i].offset);
    PutU32(file, entry + 8, toc[i].size);
    PutU32(file, entry + 12, toc[i].crc);
  }
  std::vector<std::byte> checked(file.begin(), file.begin() + kHeaderCrcOffset);
  checked.insert(checked.end(), file.begin() + kHeaderSize,
                 file.begin() + static_cast<ptrdiff_t>(
                                    kHeaderSize + toc.size() * kTocEntrySize));
  PutU32(file, kHeaderCrcOffset, scene::Crc32(checked));
  return BytesResult::Ok(std::move(file));
}

PrefabResult DecodePrefab(std::vector<std::byte> file,
                          std::string_view base_dir,
                          const LoadOptions& options) {
  Prefab prefab;
  // The prefab's spans view this block, so parse from the adopted copy.
  const std::span<const std::byte> data = prefab.storage.Adopt(std::move(file));
  auto toc = ReadToc(data);
  if (!toc.ok()) {
    return PrefabResult::Error(toc.error());
  }

  std::span<const std::byte> bulk;
  bool has_bulk = false;
  for (const TocEntry& entry : toc.value()) {
    if (entry.fourcc == kBulkChunk) {
      if (has_bulk) {
        return PrefabResult::Error(PrefabError::kCorrupt);
      }
      has_bulk = true;
      bulk = data.subspan(entry.offset, entry.size);
      if (options.verify_bulk_checksum && scene::Crc32(bulk) != entry.crc) {
        return PrefabResult::Error(PrefabError::kChecksumMismatch);
      }
    }
  }

  std::vector<uint32_t> seen;
  for (const TocEntry& entry : toc.value()) {
    if (entry.fourcc == kBulkChunk) {
      continue;
    }
    if (std::ranges::find(seen, entry.fourcc) != seen.end()) {
      return PrefabResult::Error(PrefabError::kCorrupt);
    }
    seen.push_back(entry.fourcc);
    const std::span<const std::byte> body =
        data.subspan(entry.offset, entry.size);
    if (scene::Crc32(body) != entry.crc) {
      return PrefabResult::Error(PrefabError::kChecksumMismatch);
    }
    if (Status decoded =
            DecodeChunk(entry.fourcc, body, bulk, base_dir, prefab);
        !decoded.ok()) {
      return PrefabResult::Error(decoded.error());
    }
  }

  if (ValidatePrefab(prefab).has_value()) {
    return PrefabResult::Error(PrefabError::kCorrupt);
  }
  return PrefabResult::Ok(std::move(prefab));
}

core::Result<void, PrefabError> SavePrefab(const Prefab& prefab,
                                           const std::string& path) {
  using SaveResult = core::Result<void, PrefabError>;
  const auto start = std::chrono::steady_clock::now();
  const std::string base_dir = core::ParentPath(platform::AbsolutePath(path));
  BytesResult encoded = EncodePrefab(prefab, base_dir);
  if (!encoded.ok()) {
    return SaveResult::Error(encoded.error());
  }
  if (!platform::WriteFileAtomic(path, encoded.value()).ok()) {
    return SaveResult::Error(PrefabError::kWriteFailed);
  }
  core::log::Info(
      "Saved prefab {} ({:.2f} MB) in {:.1f} ms", path,
      static_cast<double>(encoded.value().size()) / (1024.0 * 1024.0),
      MillisecondsSince(start));
  return SaveResult::Ok();
}

PrefabResult LoadPrefab(const std::string& path, const LoadOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  auto file = platform::ReadFile(path, kMaxPrefabFileSize);
  if (!file.ok()) {
    return PrefabResult::Error(PrefabError::kCannotOpen);
  }
  const double read_ms = MillisecondsSince(start);
  const size_t size = file.value().size();
  const std::string base_dir = core::ParentPath(platform::AbsolutePath(path));
  PrefabResult prefab =
      DecodePrefab(std::move(file).value(), base_dir, options);
  if (!prefab.ok()) {
    return prefab;
  }
  core::log::Info(
      "Loaded prefab {} ({:.2f} MB) in {:.1f} ms (read {:.1f} ms, decode "
      "{:.1f} ms)",
      path, static_cast<double>(size) / (1024.0 * 1024.0),
      MillisecondsSince(start), read_ms, MillisecondsSince(start) - read_ms);
  return prefab;
}

}  // namespace blue::prefab
