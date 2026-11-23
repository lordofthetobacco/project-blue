#include "cook/texture_cook.h"

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "cook/internal/parallel.h"
#include "core/log.h"

namespace blue::cook {
namespace {

using asset::ImageData;
using prefab::TextureFormat;

constexpr int kBlockSize = 4;
constexpr size_t kBlockBytes = 16;

// A linear float RGBA image used while filtering mips: colour in linear
// light, data as 0..1, normals as -1..1 vectors.
struct FloatImage {
  int width = 0;
  int height = 0;
  std::vector<float> rgba;
};

float SrgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// Returns the linear value of every 8-bit sRGB code.
const std::array<float, 256>& SrgbDecodeTable() {
  static const std::array<float, 256> table = [] {
    std::array<float, 256> values{};
    for (size_t i = 0; i < values.size(); ++i) {
      values[i] = SrgbToLinear(static_cast<float>(i) / 255.0f);
    }
    return values;
  }();
  return table;
}

// Returns, for k = 0..254, the linear value at which the sRGB encoding rounds
// up to code k + 1: SrgbToLinear((k + 0.5) / 255). Counting thresholds below
// a value gives its correctly rounded code without evaluating pow.
const std::array<float, 255>& SrgbEncodeThresholds() {
  static const std::array<float, 255> table = [] {
    std::array<float, 255> values{};
    for (size_t k = 0; k < values.size(); ++k) {
      values[k] = SrgbToLinear((static_cast<float>(k) + 0.5f) / 255.0f);
    }
    return values;
  }();
  return table;
}

uint8_t EncodeSrgb(float linear) {
  const std::array<float, 255>& thresholds = SrgbEncodeThresholds();
  return static_cast<uint8_t>(
      std::upper_bound(thresholds.begin(), thresholds.end(), linear) -
      thresholds.begin());
}

uint8_t EncodeUnorm(float value) {
  return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

float DecodeSnorm(uint8_t value) {
  return static_cast<float>(value) / 255.0f * 2.0f - 1.0f;
}

uint8_t EncodeSnorm(float value) { return EncodeUnorm(value * 0.5f + 0.5f); }

// Converts one value of channel `channel` (3 = alpha) to filtering space.
float DecodeValue(uint8_t value, size_t channel, TextureUsage usage,
                  const float* srgb) {
  if (channel == 3 || usage == TextureUsage::kLinear) {
    return static_cast<float>(value) / 255.0f;
  }
  return usage == TextureUsage::kColor ? srgb[value] : DecodeSnorm(value);
}

uint8_t EncodeValue(float value, size_t channel, TextureUsage usage) {
  if (channel == 3 || usage == TextureUsage::kLinear) {
    return EncodeUnorm(value);
  }
  return usage == TextureUsage::kColor ? EncodeSrgb(value) : EncodeSnorm(value);
}

// The per-pixel loops below run one row per work item on all cores and use
// raw row pointers: this code is compiled without optimisation in Debug,
// where checked iterators would dominate the cook time.

// Normalises the xyz of the `count` RGBA texels at `texels`; zero vectors
// become +Z.
void NormalizeVectors(float* texels, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    float* v = texels + i * 4;
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1.0e-6f) {
      v[0] /= length;
      v[1] /= length;
      v[2] /= length;
    } else {
      v[0] = 0.0f;
      v[1] = 0.0f;
      v[2] = 1.0f;
    }
  }
}

// Converts the `width` RGBA texels at `src` to filtering space; normal
// vectors are renormalised.
void DecodeRow(const uint8_t* src, size_t width, TextureUsage usage,
               float* dst) {
  const float* srgb = SrgbDecodeTable().data();
  const size_t values = width * 4;
  for (size_t i = 0; i < values; ++i) {
    dst[i] = DecodeValue(src[i], i % 4, usage, srgb);
  }
  if (usage == TextureUsage::kNormal) {
    NormalizeVectors(dst, width);
  }
}

void EncodeRow(const float* src, size_t width, TextureUsage usage,
               uint8_t* dst) {
  const size_t values = width * 4;
  for (size_t i = 0; i < values; ++i) {
    dst[i] = EncodeValue(src[i], i % 4, usage);
  }
}

ImageData ToBytes(const FloatImage& image, TextureUsage usage) {
  ImageData result{.width = image.width, .height = image.height};
  result.pixels.resize(image.rgba.size());
  const size_t width = static_cast<size_t>(image.width);
  internal::ParallelFor(image.height, [&](int y) {
    EncodeRow(image.rgba.data() + y * width * 4, width, usage,
              result.pixels.data() + y * width * 4);
  });
  return result;
}

// Replaces the RGBA8 source `image` by its level-0 bytes: each texel goes
// to filtering space and back (renormalising normal vectors), exactly as a
// float level is written by ToBytes(), without a float copy of the image.
void ToLevelBytesInPlace(ImageData& image, TextureUsage usage) {
  const size_t width = static_cast<size_t>(image.width);
  internal::ParallelFor(image.height, [&](int y) {
    std::vector<float> row(width * 4);
    uint8_t* texels = image.pixels.data() + y * width * 4;
    DecodeRow(texels, width, usage, row.data());
    EncodeRow(row.data(), width, usage, texels);
  });
}

// The rows of one mip level in filtering space: either the RGBA8 source
// image, converted row by row on demand, or a float level as is.
class LevelRows {
 public:
  LevelRows(const ImageData& source, TextureUsage usage)
      : bytes_(&source),
        usage_(usage),
        width_(source.width),
        height_(source.height) {}
  explicit LevelRows(const FloatImage& level)
      : floats_(&level), width_(level.width), height_(level.height) {}

  int width() const { return width_; }
  int height() const { return height_; }

  // Returns row `y` (width() * 4 values); converted rows are written to
  // `scratch`, which must hold width() * 4 floats.
  const float* Row(int y, float* scratch) const {
    const size_t values = static_cast<size_t>(width_) * 4;
    if (floats_ != nullptr) {
      return floats_->rgba.data() + y * values;
    }
    DecodeRow(bytes_->pixels.data() + y * values, static_cast<size_t>(width_),
              usage_, scratch);
    return scratch;
  }

 private:
  const ImageData* bytes_ = nullptr;
  const FloatImage* floats_ = nullptr;
  TextureUsage usage_ = TextureUsage::kLinear;
  int width_ = 0;
  int height_ = 0;
};

// Source texels and weights of one destination texel along one axis.
struct Taps {
  int first = 0;
  int count = 0;
  std::array<float, 4> weights{};
};

// Returns the box filter taps for shrinking an axis of `source` texels to
// `target` texels (target <= source < 2 * target + 2): destination texel i
// covers [i * r, (i + 1) * r) of the source with r = source / target, and
// each source texel is weighted by its overlap with that interval.
std::vector<Taps> BoxTaps(int source, int target) {
  std::vector<Taps> taps(static_cast<size_t>(target));
  const double ratio = static_cast<double>(source) / target;
  for (int i = 0; i < target; ++i) {
    const double begin = i * ratio;
    const double end = (i + 1) * ratio;
    Taps& tap = taps[static_cast<size_t>(i)];
    tap.first = static_cast<int>(std::floor(begin));
    const int last = std::min(source - 1, static_cast<int>(std::ceil(end)) - 1);
    tap.count =
        std::min(static_cast<int>(tap.weights.size()), last - tap.first + 1);
    for (int k = 0; k < tap.count; ++k) {
      const double texel = tap.first + k;
      const double overlap =
          std::min(end, texel + 1.0) - std::max(begin, texel);
      tap.weights[static_cast<size_t>(k)] =
          static_cast<float>(std::max(0.0, overlap) / ratio);
    }
  }
  return taps;
}

// Returns `source` halved in both directions (each side to max(1, side / 2))
// with an exact box filter, run separably. Works in bands of destination
// rows, filtering the source rows they cover horizontally on the fly, so no
// intermediate image of the whole level is made (a converted RGBA8 source is
// never held as floats either). Same arithmetic, in the same order, as a
// full horizontal pass followed by a vertical one.
FloatImage Downsample(const LevelRows& source, TextureUsage usage) {
  constexpr int kBandRows = 8;
  const int width = std::max(1, source.width() / 2);
  const int height = std::max(1, source.height() / 2);
  const std::vector<Taps> taps_x = BoxTaps(source.width(), width);
  const std::vector<Taps> taps_y = BoxTaps(source.height(), height);
  const size_t source_row = static_cast<size_t>(source.width()) * 4;
  const size_t row = static_cast<size_t>(width) * 4;

  FloatImage result{.width = width, .height = height};
  result.rgba.assign(row * height, 0.0f);
  const int bands = (height + kBandRows - 1) / kBandRows;
  internal::ParallelFor(bands, [&](int band) {
    std::vector<float> scratch(source_row);
    std::vector<float> filtered(row);
    const int last = std::min(height, (band + 1) * kBandRows);
    for (int y = band * kBandRows; y < last; ++y) {
      const Taps& tap_y = taps_y[static_cast<size_t>(y)];
      float* dst = result.rgba.data() + y * row;
      for (int ky = 0; ky < tap_y.count; ++ky) {
        // Horizontal pass of source row tap_y.first + ky.
        const float* src = source.Row(tap_y.first + ky, scratch.data());
        for (int x = 0; x < width; ++x) {
          const Taps& tap = taps_x[static_cast<size_t>(x)];
          float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
          for (int k = 0; k < tap.count; ++k) {
            const float* texel = src + static_cast<size_t>(tap.first + k) * 4;
            const float weight = tap.weights[static_cast<size_t>(k)];
            for (size_t c = 0; c < 4; ++c) {
              sum[c] += texel[c] * weight;
            }
          }
          std::memcpy(filtered.data() + static_cast<size_t>(x) * 4, sum,
                      sizeof(sum));
        }
        // Vertical accumulation.
        const float weight = tap_y.weights[static_cast<size_t>(ky)];
        for (size_t i = 0; i < row; ++i) {
          dst[i] += filtered[i] * weight;
        }
      }
      if (usage == TextureUsage::kNormal) {
        NormalizeVectors(dst, static_cast<size_t>(width));
      }
    }
  });
  return result;
}

TextureFormat FormatFor(const TextureCookOptions& options) {
  switch (options.usage) {
    case TextureUsage::kColor:
      return options.compress ? TextureFormat::kBc7Srgb
                              : TextureFormat::kRgba8Srgb;
    case TextureUsage::kLinear:
      return options.compress ? TextureFormat::kBc7Unorm
                              : TextureFormat::kRgba8Unorm;
    case TextureUsage::kNormal:
      return options.compress ? TextureFormat::kBc5Unorm
                              : TextureFormat::kRgba8Unorm;
  }
  return TextureFormat::kRgba8Unorm;
}

// Initialises the encoders' lookup tables once per process.
void InitEncoders() {
  static std::once_flag once;
  std::call_once(once, [] {
    bc7enc_compress_block_init();
    rgbcx::init();
  });
}

bc7enc_compress_block_params Bc7Params(const TextureCookOptions& options) {
  bc7enc_compress_block_params params;
  bc7enc_compress_block_params_init(&params);
  if (options.usage == TextureUsage::kColor) {
    bc7enc_compress_block_params_init_perceptual_weights(&params);
  } else {
    bc7enc_compress_block_params_init_linear_weights(&params);
  }
  if (options.quality == CookQuality::kHigh) {
    params.m_uber_level = 2;
    params.m_max_partitions = BC7ENC_MAX_PARTITIONS;
  } else {
    params.m_uber_level = 0;
    params.m_max_partitions = 16;
  }
  return params;
}

// Copies the 4x4 block at block (bx, by) of `image` into `block` (RGBA8),
// repeating the last row and column past the image edge.
void GatherBlock(const ImageData& image, int bx, int by,
                 std::array<uint8_t, 64>& block) {
  for (int y = 0; y < kBlockSize; ++y) {
    const int sy = std::min(by * kBlockSize + y, image.height - 1);
    for (int x = 0; x < kBlockSize; ++x) {
      const int sx = std::min(bx * kBlockSize + x, image.width - 1);
      const size_t source = (static_cast<size_t>(sy) * image.width + sx) * 4;
      std::memcpy(&block[static_cast<size_t>(y * kBlockSize + x) * 4],
                  &image.pixels[source], 4);
    }
  }
}

// Encodes `level` into `out` (MipByteSize(format, ...) bytes).
void EncodeLevel(const ImageData& level, TextureFormat format,
                 const TextureCookOptions& options,
                 const bc7enc_compress_block_params& bc7,
                 std::span<std::byte> out) {
  if (!prefab::IsBlockCompressed(format)) {
    std::memcpy(out.data(), level.pixels.data(), level.pixels.size());
    return;
  }
  const int blocks_x = (level.width + kBlockSize - 1) / kBlockSize;
  const int blocks_y = (level.height + kBlockSize - 1) / kBlockSize;
  const bool high = options.quality == CookQuality::kHigh;
  internal::ParallelFor(blocks_y, [&](int by) {
    std::array<uint8_t, 64> pixels{};
    for (int bx = 0; bx < blocks_x; ++bx) {
      GatherBlock(level, bx, by, pixels);
      std::byte* block =
          out.data() + (static_cast<size_t>(by) * blocks_x + bx) * kBlockBytes;
      if (format == TextureFormat::kBc5Unorm) {
        if (high) {
          rgbcx::encode_bc5_hq(block, pixels.data());
        } else {
          rgbcx::encode_bc5(block, pixels.data());
        }
      } else {
        bc7enc_compress_block(block, pixels.data(), &bc7);
      }
    }
  });
}

const char* UsageName(TextureUsage usage) {
  switch (usage) {
    case TextureUsage::kColor:
      return "colour";
    case TextureUsage::kLinear:
      return "linear";
    case TextureUsage::kNormal:
      return "normal";
  }
  return "?";
}

// Returns the channel `channel` of `image` at normalised (u, v), bilinear
// between texel centres, clamped at the edges.
float SampleChannel(const ImageData& image, int channel, float u, float v) {
  const float x = std::clamp(u * image.width - 0.5f, 0.0f,
                             static_cast<float>(image.width - 1));
  const float y = std::clamp(v * image.height - 0.5f, 0.0f,
                             static_cast<float>(image.height - 1));
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, image.width - 1);
  const int y1 = std::min(y0 + 1, image.height - 1);
  const float fx = x - static_cast<float>(x0);
  const float fy = y - static_cast<float>(y0);
  const auto at = [&](int px, int py) {
    return static_cast<float>(
        image.pixels[(static_cast<size_t>(py) * image.width + px) * 4 +
                     static_cast<size_t>(channel)]);
  };
  const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
  const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
  return top + (bottom - top) * fy;
}

// Calls `visit(level)` (an ImageData&, which it may move from) for every
// mip level of `image`, level 0 first: `image` halved while it exceeds
// `max_size` (> 0), then each level filtered from the previous one in
// filtering space, down to 1 x 1; only level 0 unless `all_levels`. Takes
// `image` over and reuses its pixels for level 0. Alive at any time: the
// current level, the next one as floats (half the source's byte size for
// level 1) and the level being visited.
template <typename Visit>
void ForEachMip(ImageData image, TextureUsage usage, int max_size,
                bool all_levels, const Visit& visit) {
  bool at_source = true;  // the current level is `image`, else `level`
  FloatImage level;
  const auto downsample = [&] {
    return at_source ? Downsample(LevelRows(image, usage), usage)
                     : Downsample(LevelRows(level), usage);
  };
  const auto width = [&] { return at_source ? image.width : level.width; };
  const auto height = [&] { return at_source ? image.height : level.height; };
  while (max_size > 0 && (width() > max_size || height() > max_size)) {
    level = downsample();
    if (at_source) {
      image = {};
      at_source = false;
    }
  }
  for (;;) {
    const bool last = !all_levels || (width() == 1 && height() == 1);
    FloatImage next;
    if (!last) {
      next = downsample();
    }
    if (at_source) {
      ToLevelBytesInPlace(image, usage);
      visit(image);
      image = {};
      at_source = false;
    } else {
      ImageData bytes = ToBytes(level, usage);
      level = {};
      visit(bytes);
    }
    if (last) {
      return;
    }
    level = std::move(next);
  }
}

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - start)
      .count();
}

}  // namespace

std::vector<ImageData> GenerateMipChain(const ImageData& image,
                                        TextureUsage usage, int max_size) {
  std::vector<ImageData> chain;
  ForEachMip(image, usage, max_size, true,
             [&](ImageData& level) { chain.push_back(std::move(level)); });
  return chain;
}

core::Result<EncodedTexture, CookError> EncodeTexture(
    ImageData image, const TextureCookOptions& options) {
  using Result = core::Result<EncodedTexture, CookError>;
  if (image.width <= 0 || image.height <= 0 ||
      image.pixels.size() !=
          static_cast<size_t>(image.width) * image.height * 4) {
    return Result::Error(CookError::kInvalidImage);
  }
  if (image.width > asset::kMaxImageDimension ||
      image.height > asset::kMaxImageDimension) {
    return Result::Error(CookError::kTooLarge);
  }
  const auto start = std::chrono::steady_clock::now();
  InitEncoders();

  // The level sizes are known up front, so the output is laid out first and
  // every level is encoded into it as soon as it is filtered.
  int width = image.width;
  int height = image.height;
  while (options.max_size > 0 &&
         (width > options.max_size || height > options.max_size)) {
    width = std::max(1, width / 2);
    height = std::max(1, height / 2);
  }
  const TextureFormat format = FormatFor(options);
  const int level_count =
      options.generate_mips ? prefab::FullMipCount(width, height) : 1;
  EncodedTexture result{
      .texture = {.format = format, .width = width, .height = height}};
  result.texture.mips.reserve(static_cast<size_t>(level_count));
  result.mip_offsets.reserve(static_cast<size_t>(level_count));
  size_t total = 0;
  for (int i = 0; i < level_count; ++i) {
    const int level_width = std::max(1, width >> i);
    const int level_height = std::max(1, height >> i);
    result.texture.mips.push_back(
        {.width = level_width, .height = level_height});
    result.mip_offsets.push_back(total);
    const size_t size = prefab::MipByteSize(format, level_width, level_height);
    total += (size + prefab::kBulkAlignment - 1) / prefab::kBulkAlignment *
             prefab::kBulkAlignment;
  }
  result.bytes.resize(total);

  const bc7enc_compress_block_params bc7 = Bc7Params(options);
  size_t index = 0;
  double encode_ms = 0.0;
  ForEachMip(std::move(image), options.usage, options.max_size,
             options.generate_mips, [&](const ImageData& level) {
               const auto encode_start = std::chrono::steady_clock::now();
               EncodeLevel(level, format, options, bc7,
                           std::span(result.bytes)
                               .subspan(result.mip_offsets[index],
                                        prefab::MipByteSize(format, level.width,
                                                            level.height)));
               encode_ms += MillisecondsSince(encode_start);
               ++index;
             });

  const double total_ms = MillisecondsSince(start);
  core::log::Info(
      "cooked {} texture {} x {}: {} mips, {} KiB, {} in {:.1f} ms (mips "
      "{:.1f} ms)",
      UsageName(options.usage), width, height, level_count, total / 1024,
      options.compress
          ? (options.quality == CookQuality::kHigh ? "high" : "fast")
          : "uncompressed",
      total_ms, total_ms - encode_ms);
  return Result::Ok(std::move(result));
}

prefab::Texture StoreTexture(EncodedTexture encoded,
                             prefab::BulkStorage& storage) {
  const std::span<const std::byte> block =
      storage.Adopt(std::move(encoded.bytes));
  prefab::Texture texture = std::move(encoded.texture);
  for (size_t i = 0; i < texture.mips.size(); ++i) {
    prefab::TextureMip& mip = texture.mips[i];
    mip.data = block.subspan(
        encoded.mip_offsets[i],
        prefab::MipByteSize(texture.format, mip.width, mip.height));
  }
  return texture;
}

core::Result<prefab::Texture, CookError> CookTexture(
    const ImageData& image, const TextureCookOptions& options,
    prefab::BulkStorage& storage) {
  using Result = core::Result<prefab::Texture, CookError>;
  auto encoded = EncodeTexture(image, options);
  if (!encoded.ok()) {
    return Result::Error(encoded.error());
  }
  return Result::Ok(StoreTexture(std::move(encoded).value(), storage));
}

ImageData PackOrm(const OrmSources& sources) {
  const std::array<std::pair<const ImageData*, int>, 3> inputs = {
      std::pair{sources.occlusion, sources.occlusion_channel},
      std::pair{sources.roughness, sources.roughness_channel},
      std::pair{sources.metallic, sources.metallic_channel}};
  ImageData result;
  for (const auto& [image, channel] : inputs) {
    if (image != nullptr && image->width > 0 && image->height > 0) {
      result.width = std::max(result.width, image->width);
      result.height = std::max(result.height, image->height);
    }
  }
  if (result.width == 0) {
    return {};
  }
  result.pixels.assign(static_cast<size_t>(result.width) * result.height * 4,
                       255);
  for (size_t c = 0; c < inputs.size(); ++c) {
    const auto [image, channel] = inputs[c];
    if (image == nullptr || image->width <= 0 || image->height <= 0 ||
        image->pixels.size() !=
            static_cast<size_t>(image->width) * image->height * 4) {
      continue;
    }
    const int source_channel = std::clamp(channel, 0, 3);
    const bool same_size =
        image->width == result.width && image->height == result.height;
    internal::ParallelFor(result.height, [&](int y) {
      for (int x = 0; x < result.width; ++x) {
        const size_t pixel = static_cast<size_t>(y) * result.width + x;
        uint8_t value = 0;
        if (same_size) {
          value =
              image->pixels[pixel * 4 + static_cast<size_t>(source_channel)];
        } else {
          const float u = (static_cast<float>(x) + 0.5f) / result.width;
          const float v = (static_cast<float>(y) + 0.5f) / result.height;
          value = static_cast<uint8_t>(
              SampleChannel(*image, source_channel, u, v) + 0.5f);
        }
        result.pixels[pixel * 4 + c] = value;
      }
    });
  }
  return result;
}

ImageData DecodeTextureMip(const prefab::Texture& texture, int mip) {
  if (mip < 0 || mip >= texture.mip_count()) {
    return {};
  }
  const prefab::TextureMip& level = texture.mips[static_cast<size_t>(mip)];
  ImageData image{.width = level.width, .height = level.height};
  const size_t pixel_bytes =
      static_cast<size_t>(level.width) * level.height * 4;
  if (!prefab::IsBlockCompressed(texture.format)) {
    if (level.data.size() < pixel_bytes) {
      return {};
    }
    image.pixels.resize(pixel_bytes);
    std::memcpy(image.pixels.data(), level.data.data(), pixel_bytes);
    return image;
  }
  const int blocks_x = (level.width + kBlockSize - 1) / kBlockSize;
  const int blocks_y = (level.height + kBlockSize - 1) / kBlockSize;
  if (level.data.size() <
      static_cast<size_t>(blocks_x) * blocks_y * kBlockBytes) {
    return {};
  }
  image.pixels.resize(pixel_bytes);
  for (int by = 0; by < blocks_y; ++by) {
    for (int bx = 0; bx < blocks_x; ++bx) {
      const std::byte* block =
          level.data.data() +
          (static_cast<size_t>(by) * blocks_x + bx) * kBlockBytes;
      std::array<uint8_t, 64> pixels{};
      if (texture.format == TextureFormat::kBc5Unorm) {
        rgbcx::unpack_bc5(block, pixels.data());
        for (size_t i = 0; i < 16; ++i) {
          const float x = DecodeSnorm(pixels[i * 4]);
          const float y = DecodeSnorm(pixels[i * 4 + 1]);
          pixels[i * 4 + 2] =
              EncodeSnorm(std::sqrt(std::max(0.0f, 1.0f - x * x - y * y)));
          pixels[i * 4 + 3] = 255;
        }
      } else {
        std::array<bc7decomp::color_rgba, 16> colors{};
        bc7decomp::unpack_bc7(block, colors.data());
        std::memcpy(pixels.data(), colors.data(), pixels.size());
      }
      for (int y = 0; y < kBlockSize; ++y) {
        const int py = by * kBlockSize + y;
        if (py >= level.height) {
          break;
        }
        for (int x = 0; x < kBlockSize; ++x) {
          const int px = bx * kBlockSize + x;
          if (px >= level.width) {
            break;
          }
          std::memcpy(
              &image.pixels[(static_cast<size_t>(py) * level.width + px) * 4],
              &pixels[static_cast<size_t>(y * kBlockSize + x) * 4], 4);
        }
      }
    }
  }
  return image;
}

}  // namespace blue::cook
