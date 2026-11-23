#include "render/metal/image.h"

#include <algorithm>
#include <vector>

#include "core/log.h"
#include "render/metal/metal_check.h"
#include "render/metal/staging_upload.h"

namespace blue::render::metal {

namespace {

constexpr uint32_t kCubeFaces = 6;

MTLTextureType TextureType(const ImageDesc& desc) {
  if (desc.cube) {
    return MTLTextureTypeCube;
  }
  return desc.array_layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
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
  resident_ = Resident(context, texture_);
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

  StagingUploader uploader(context, expected);
  for (uint32_t layer = 0; layer < desc.array_layers; ++layer) {
    uploader.CopyToImage(texels.subspan(layer * layer_bytes, layer_bytes),
                         image.texture(), 0, layer);
  }
  if (image.mip_levels() > 1) {
    [uploader.encoder() generateMipmapsForTexture:image.texture()];
  }
  uploader.Finish();
  return image;
}

std::vector<Image> CreateImagesFromMipChains(
    const MetalContext& context, std::span<const MipChainUpload> uploads) {
  size_t total_bytes = 0;
  for (const MipChainUpload& upload : uploads) {
    if (upload.mips.size() != upload.desc.mip_levels ||
        upload.desc.array_layers != 1) {
      core::log::Fatal("Mip chain upload: {} levels given for {}",
                       upload.mips.size(), upload.desc.mip_levels);
    }
    for (const std::span<const std::byte> level : upload.mips) {
      total_bytes += level.size_bytes();
    }
  }
  std::vector<Image> images;
  images.reserve(uploads.size());
  if (uploads.empty()) {
    return images;
  }

  // Image after image, its levels split over staging chunks as they fill
  // up. A level of the wrong size (or of a format without a copy layout) is
  // fatal in CopyToImage().
  StagingUploader uploader(context, total_bytes);
  for (const MipChainUpload& upload : uploads) {
    ImageDesc desc = upload.desc;
    desc.usage |= MTLTextureUsageShaderRead;
    if (desc.label == nullptr) {
      desc.label = "Mip chain image";
    }
    const Image& image = images.emplace_back(context, desc);
    for (uint32_t level = 0; level < desc.mip_levels; ++level) {
      uploader.CopyToImage(upload.mips[level], image.texture(), level, 0);
    }
  }
  uploader.Finish();
  return images;
}

}  // namespace blue::render::metal
