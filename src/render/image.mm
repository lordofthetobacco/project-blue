#include "render/image.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "render/buffer.h"
#include "render/metal_check.h"

namespace blue::render {

namespace {

constexpr uint32_t kCubeFaces = 6;
// Bytes of one 4 x 4 block of the block-compressed formats we upload.
constexpr uint32_t kBcBlockBytes = 16;
constexpr uint32_t kBcBlockSize = 4;

MTLTextureType TextureType(const ImageDesc& desc) {
  if (desc.cube) {
    return MTLTextureTypeCube;
  }
  return desc.array_layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
}

// Returns the extent of mip `level` of `extent`; never below 1 x 1.
core::Extent2D MipExtent(core::Extent2D extent, uint32_t level) {
  return {std::max(extent.width >> level, 1u),
          std::max(extent.height >> level, 1u)};
}

bool IsBlockCompressed(MTLPixelFormat format) {
  switch (format) {
    case MTLPixelFormatBC5_RGUnorm:
    case MTLPixelFormatBC5_RGSnorm:
    case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
      return true;
    default:
      return false;
  }
}

// The copy layout of one level: bytes per row (of texels, or of 4 x 4 blocks)
// and rows.
struct LevelLayout {
  size_t bytes_per_row = 0;
  size_t rows = 0;
};

LevelLayout LayoutOf(MTLPixelFormat format, core::Extent2D extent) {
  if (IsBlockCompressed(format)) {
    const auto blocks = [](uint32_t texels) {
      return (texels + kBcBlockSize - 1) / kBcBlockSize;
    };
    return {size_t{blocks(extent.width)} * kBcBlockBytes,
            blocks(extent.height)};
  }
  return {size_t{extent.width} * TexelSize(format), extent.height};
}

// Whether generateMipmapsForTexture works for `format`, which must have a
// texel size: it needs a color-renderable, filterable format, and 32-bit
// floats are filterable only on some devices.
bool CanGenerateMips(id<MTLDevice> device, MTLPixelFormat format) {
  switch (format) {
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA32Float:
      return device.supports32BitFloatFiltering;
    default:
      return true;
  }
}

NSString* LabelOr(const char* label, NSString* fallback) {
  return label != nullptr ? @(label) : fallback;
}

}  // namespace

uint32_t TexelSize(MTLPixelFormat format) {
  switch (format) {
    case MTLPixelFormatR8Unorm:
      return 1;
    case MTLPixelFormatRG8Unorm:
      return 2;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatR32Float:
      return 4;
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRG32Float:
      return 8;
    case MTLPixelFormatRGBA32Float:
      return 16;
    default:
      return 0;
  }
}

Image::Image(const MetalContext& context, const ImageDesc& desc) {
  if (desc.cube && (desc.array_layers != kCubeFaces ||
                    desc.extent.width != desc.extent.height)) {
    core::log::Fatal("A cube image needs 6 layers and a square extent");
  }
  MTLTextureDescriptor* const descriptor = [[MTLTextureDescriptor alloc] init];
  descriptor.textureType = TextureType(desc);
  descriptor.pixelFormat = desc.format;
  descriptor.width = desc.extent.width;
  descriptor.height = desc.extent.height;
  descriptor.mipmapLevelCount = std::max(desc.mip_levels, 1u);
  // A cube is one "array element" of 6 faces.
  descriptor.arrayLength = desc.cube ? 1 : desc.array_layers;
  descriptor.usage = desc.usage;
  descriptor.storageMode = MTLStorageModePrivate;
  texture_ = [context.device() newTextureWithDescriptor:descriptor];
  FatalIfNil(texture_, nil, "newTextureWithDescriptor");
  texture_.label = LabelOr(desc.label, @"Image");
}

ImageView Image::CreateView(const ImageViewDesc& desc) const {
  const uint32_t mips =
      desc.mip_count == 0 ? mip_levels() - desc.base_mip : desc.mip_count;
  const uint32_t layers = desc.layer_count == 0
                              ? array_layers() - desc.base_layer
                              : desc.layer_count;
  const id<MTLTexture> view =
      [texture_ newTextureViewWithPixelFormat:texture_.pixelFormat
                                  textureType:desc.type
                                       levels:NSMakeRange(desc.base_mip, mips)
                                       slices:NSMakeRange(desc.base_layer,
                                                          layers)];
  FatalIfNil(view, nil, "newTextureViewWithPixelFormat");
  view.label = [NSString stringWithFormat:@"%@ view (mip %u, layer %u)",
                                          texture_.label, desc.base_mip,
                                          desc.base_layer];
  return ImageView(view);
}

MTLPixelFormat Image::format() const { return texture_.pixelFormat; }

core::Extent2D Image::extent() const {
  return {static_cast<uint32_t>(texture_.width),
          static_cast<uint32_t>(texture_.height)};
}

uint32_t Image::array_layers() const {
  if (texture_.textureType == MTLTextureTypeCube) {
    return kCubeFaces;
  }
  return static_cast<uint32_t>(texture_.arrayLength);
}

uint32_t Image::mip_levels() const {
  return static_cast<uint32_t>(texture_.mipmapLevelCount);
}

uint64_t Image::size_bytes() const { return texture_.allocatedSize; }

Image CreateSampledImage(const MetalContext& context, ImageDesc desc,
                         std::span<const std::byte> texels) {
  const size_t row_bytes = size_t{TexelSize(desc.format)} * desc.extent.width;
  const size_t layer_bytes = row_bytes * desc.extent.height;
  const size_t expected = layer_bytes * desc.array_layers;
  if (expected == 0 || texels.size_bytes() != expected) {
    core::log::Fatal("Texture upload of {} bytes, expected {} (format {})",
                     texels.size_bytes(), expected,
                     static_cast<int>(desc.format));
  }
  if (desc.mip_levels > 1 &&
      !CanGenerateMips(context.device(), desc.format)) {
    core::log::Warn("Format {} cannot be filtered: texture gets no mips",
                    static_cast<int>(desc.format));
    desc.mip_levels = 1;
  }
  // generateMipmapsForTexture needs no usage flag (only a color-renderable,
  // filterable format, checked above), and copies into a texture need none
  // either: sampling is the only usage the upload adds.
  desc.usage |= MTLTextureUsageShaderRead;
  if (desc.label == nullptr) {
    desc.label = "Sampled image";
  }
  Image image(context, desc);

  Buffer staging(context, expected, Buffer::Access::kCpuSequentialWrite,
                 "Texture staging");
  std::memcpy(staging.mapped(), texels.data(), expected);

  context.SubmitImmediate([&](id<MTLCommandBuffer> command_buffer) {
    const id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"Upload texture";
    for (uint32_t layer = 0; layer < desc.array_layers; ++layer) {
      [blit copyFromBuffer:staging.handle()
                 sourceOffset:layer * layer_bytes
            sourceBytesPerRow:row_bytes
          sourceBytesPerImage:layer_bytes
                   sourceSize:MTLSizeMake(desc.extent.width,
                                          desc.extent.height, 1)
                    toTexture:image.texture()
             destinationSlice:layer
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    if (image.mip_levels() > 1) {
      [blit generateMipmapsForTexture:image.texture()];
    }
    [blit endEncoding];
  });
  return image;
}

std::vector<Image> CreateImagesFromMipChains(
    const MetalContext& context, std::span<const MipChainUpload> uploads) {
  // Offsets of levels in the staging buffer are aligned to a BC block
  // (16 bytes), which also satisfies the texel size of every other format.
  constexpr size_t kLevelAlignment = kBcBlockBytes;
  const auto align = [](size_t offset) {
    return (offset + kLevelAlignment - 1) / kLevelAlignment * kLevelAlignment;
  };
  size_t staging_size = 0;
  for (const MipChainUpload& upload : uploads) {
    if (upload.mips.size() != upload.desc.mip_levels ||
        upload.desc.array_layers != 1) {
      core::log::Fatal("Mip chain upload: {} levels given for {}",
                       upload.mips.size(), upload.desc.mip_levels);
    }
    if (!IsBlockCompressed(upload.desc.format) &&
        TexelSize(upload.desc.format) == 0) {
      core::log::Fatal("Mip chain upload: unsupported format {}",
                       static_cast<int>(upload.desc.format));
    }
    for (const std::span<const std::byte> level : upload.mips) {
      staging_size = align(staging_size) + level.size_bytes();
    }
  }
  std::vector<Image> images;
  images.reserve(uploads.size());
  if (uploads.empty()) {
    return images;
  }

  Buffer staging(context, std::max(staging_size, size_t{1}),
                 Buffer::Access::kCpuSequentialWrite, "Mip chain staging");
  auto* const staging_bytes = static_cast<std::byte*>(staging.mapped());
  size_t offset = 0;
  // Where each level's bytes landed in the staging buffer, image after image,
  // level 0 first.
  std::vector<size_t> level_offsets;
  for (const MipChainUpload& upload : uploads) {
    ImageDesc desc = upload.desc;
    desc.usage |= MTLTextureUsageShaderRead;
    if (desc.label == nullptr) {
      desc.label = "Mip chain image";
    }
    images.emplace_back(context, desc);
    for (uint32_t level = 0; level < desc.mip_levels; ++level) {
      const std::span<const std::byte> bytes = upload.mips[level];
      const LevelLayout layout =
          LayoutOf(desc.format, MipExtent(desc.extent, level));
      if (bytes.size_bytes() < layout.bytes_per_row * layout.rows) {
        core::log::Fatal("Mip chain upload: level {} has {} bytes, needs {}",
                         level, bytes.size_bytes(),
                         layout.bytes_per_row * layout.rows);
      }
      offset = align(offset);
      std::memcpy(staging_bytes + offset, bytes.data(), bytes.size_bytes());
      level_offsets.push_back(offset);
      offset += bytes.size_bytes();
    }
  }

  context.SubmitImmediate([&](id<MTLCommandBuffer> command_buffer) {
    const id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    blit.label = @"Upload mip chains";
    size_t next_level = 0;
    for (const Image& image : images) {
      for (uint32_t level = 0; level < image.mip_levels(); ++level) {
        // Levels below 4 x 4 of a BC format copy one partial block; their
        // source size is the real level size, not the block size.
        const core::Extent2D extent = MipExtent(image.extent(), level);
        const LevelLayout layout = LayoutOf(image.format(), extent);
        [blit copyFromBuffer:staging.handle()
                   sourceOffset:level_offsets[next_level++]
              sourceBytesPerRow:layout.bytes_per_row
            sourceBytesPerImage:layout.bytes_per_row * layout.rows
                     sourceSize:MTLSizeMake(extent.width, extent.height, 1)
                      toTexture:image.texture()
               destinationSlice:0
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
      }
    }
    [blit endEncoding];
  });
  return images;
}

}  // namespace blue::render
