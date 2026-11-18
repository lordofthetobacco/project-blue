#include "render/texture_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "asset/image_decoder.h"
#include "core/log.h"
#include "core/result.h"

namespace blue::render {

namespace {

using DecodeResult = core::Result<asset::ImageData, asset::ImageError>;

// Images decoded by one worker with one asset::LoadTextures call: a single
// image file, or some embedded textures of one model file (parsed once per
// job).
struct DecodeJob {
  std::vector<material::TextureRef> refs;
  std::vector<DecodeResult> results;
};

// Returns the number of decode threads to use.
size_t WorkerCount() {
  return std::max<size_t>(1, std::thread::hardware_concurrency());
}

// Groups `refs` (distinct) into decode jobs. The embedded textures of one
// model are spread over up to WorkerCount() jobs: each job parses the model
// again, but decoding (the expensive part for large PNGs) runs in parallel.
std::vector<DecodeJob> MakeJobs(std::span<const material::TextureRef> refs) {
  std::vector<DecodeJob> jobs;
  std::vector<bool> taken(refs.size(), false);
  for (size_t i = 0; i < refs.size(); ++i) {
    if (taken[i]) {
      continue;
    }
    if (!refs[i].embedded()) {
      jobs.push_back({.refs = {refs[i]}, .results = {}});
      continue;
    }
    std::vector<material::TextureRef> model_refs;
    for (size_t j = i; j < refs.size(); ++j) {
      if (!taken[j] && refs[j].embedded() && refs[j].path == refs[i].path) {
        model_refs.push_back(refs[j]);
        taken[j] = true;
      }
    }
    const size_t per_job =
        (model_refs.size() + WorkerCount() - 1) / WorkerCount();
    for (size_t first = 0; first < model_refs.size(); first += per_job) {
      const size_t last = std::min(first + per_job, model_refs.size());
      jobs.push_back(
          {.refs = std::vector<material::TextureRef>(model_refs.begin() + first,
                                                     model_refs.begin() + last),
           .results = {}});
    }
  }
  return jobs;
}

// Runs every job, spread over worker threads.
void RunJobs(std::vector<DecodeJob>& jobs) {
  const size_t workers = std::min(jobs.size(), WorkerCount());
  std::atomic<size_t> next{0};
  const auto work = [&jobs, &next] {
    for (size_t i = next++; i < jobs.size(); i = next++) {
      jobs[i].results = asset::LoadTextures(jobs[i].refs);
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(workers > 0 ? workers - 1 : 0);
  for (size_t i = 1; i < workers; ++i) {
    threads.emplace_back(work);
  }
  work();
  for (std::thread& thread : threads) {
    thread.join();
  }
}

// Returns the decoded image of `ref` among `jobs`, or null if it failed (the
// failure is logged) or was not decoded.
const asset::ImageData* FindDecoded(const std::vector<DecodeJob>& jobs,
                                    const material::TextureRef& ref) {
  for (const DecodeJob& job : jobs) {
    for (size_t i = 0; i < job.refs.size() && i < job.results.size(); ++i) {
      if (job.refs[i] == ref) {
        return job.results[i].ok() ? &job.results[i].value() : nullptr;
      }
    }
  }
  return nullptr;
}

// Logs every failed decode of `jobs` once.
void LogFailures(const std::vector<DecodeJob>& jobs) {
  for (const DecodeJob& job : jobs) {
    for (size_t i = 0; i < job.refs.size() && i < job.results.size(); ++i) {
      if (!job.results[i].ok()) {
        const material::TextureRef& ref = job.refs[i];
        core::log::Warn("Texture '{}'{}: {}; using a default", ref.path,
                        ref.embedded()
                            ? " #" + std::to_string(ref.embedded_index)
                            : std::string(),
                        asset::ToString(job.results[i].error()));
      }
    }
  }
}

Image UploadTexture(const VulkanContext& context, const asset::ImageData& image,
                    VkFormat format) {
  const VkExtent2D extent{static_cast<uint32_t>(image.width),
                          static_cast<uint32_t>(image.height)};
  return CreateSampledImage(
      context,
      {.extent = extent, .format = format, .mip_levels = MipLevelCount(extent)},
      std::as_bytes(std::span(image.pixels)));
}

Image MakeSolidTexture(const VulkanContext& context,
                       std::array<uint8_t, 4> rgba) {
  return CreateSampledImage(
      context, {.extent = {1, 1}, .format = VK_FORMAT_R8G8B8A8_UNORM},
      std::as_bytes(std::span(rgba)));
}

void HashCombine(size_t& seed, size_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

}  // namespace

size_t TextureCache::KeyHash::operator()(const Key& key) const {
  size_t seed = std::hash<std::string>{}(key.primary.path);
  HashCombine(seed, std::hash<int>{}(key.primary.embedded_index));
  HashCombine(seed, std::hash<std::string>{}(key.secondary.path));
  HashCombine(seed, std::hash<int>{}(key.secondary.embedded_index));
  HashCombine(seed, static_cast<size_t>(key.kind));
  return seed;
}

TextureCache::TextureCache(const VulkanContext& context) : context_(context) {
  defaults_[static_cast<size_t>(DefaultTexture::kWhite)] =
      MakeSolidTexture(context_, {255, 255, 255, 255});
  defaults_[static_cast<size_t>(DefaultTexture::kFlatNormal)] =
      MakeSolidTexture(context_, {128, 128, 255, 255});
  defaults_[static_cast<size_t>(DefaultTexture::kBlack)] =
      MakeSolidTexture(context_, {0, 0, 0, 255});
}

void TextureCache::Preload(std::span<const material::MaterialDesc> materials) {
  std::vector<Key> keys;
  const auto add = [&keys](const material::TextureRef& ref, TextureKind kind) {
    if (!ref.empty()) {
      keys.push_back({.primary = ref, .secondary = {}, .kind = kind});
    }
  };
  for (const material::MaterialDesc& desc : materials) {
    add(desc.base_color_texture, TextureKind::kColor);
    add(desc.emissive_texture, TextureKind::kColor);
    add(desc.normal_texture, TextureKind::kData);
    add(desc.occlusion_texture, TextureKind::kData);
    if (!desc.metallic_roughness_texture.empty()) {
      add(desc.metallic_roughness_texture, TextureKind::kData);
    } else if (!desc.roughness_texture.empty() ||
               !desc.metallic_texture.empty()) {
      keys.push_back({.primary = desc.roughness_texture,
                      .secondary = desc.metallic_texture,
                      .kind = TextureKind::kPackedMetallicRoughness});
    }
  }
  Load(keys);
}

VkImageView TextureCache::Get(const material::TextureRef& ref, TextureKind kind,
                              DefaultTexture fallback) {
  if (ref.empty()) {
    return default_view(fallback);
  }
  const Key key{.primary = ref, .secondary = {}, .kind = kind};
  Load({&key, 1});
  return ViewOf(key, fallback);
}

VkImageView TextureCache::GetMetallicRoughness(
    const material::MaterialDesc& desc) {
  if (!desc.metallic_roughness_texture.empty()) {
    return Get(desc.metallic_roughness_texture, TextureKind::kData,
               DefaultTexture::kWhite);
  }
  if (desc.roughness_texture.empty() && desc.metallic_texture.empty()) {
    return default_view(DefaultTexture::kWhite);
  }
  const Key key{.primary = desc.roughness_texture,
                .secondary = desc.metallic_texture,
                .kind = TextureKind::kPackedMetallicRoughness};
  Load({&key, 1});
  return ViewOf(key, DefaultTexture::kWhite);
}

VkImageView TextureCache::ViewOf(const Key& key,
                                 DefaultTexture fallback) const {
  const auto found = textures_.find(key);
  if (found == textures_.end() || found->second.handle() == VK_NULL_HANDLE) {
    return default_view(fallback);
  }
  return found->second.view();
}

void TextureCache::Load(std::span<const Key> keys) {
  std::vector<Key> missing;
  std::vector<material::TextureRef> refs;
  const auto need = [&refs](const material::TextureRef& ref) {
    if (!ref.empty() && std::ranges::find(refs, ref) == refs.end()) {
      refs.push_back(ref);
    }
  };
  for (const Key& key : keys) {
    if (textures_.contains(key) ||
        std::ranges::find(missing, key) != missing.end()) {
      continue;
    }
    missing.push_back(key);
    need(key.primary);
    need(key.secondary);
  }
  if (missing.empty()) {
    return;
  }

  const auto start = std::chrono::steady_clock::now();
  std::vector<DecodeJob> jobs = MakeJobs(refs);
  RunJobs(jobs);
  LogFailures(jobs);
  const auto decoded = std::chrono::steady_clock::now();

  size_t loaded = 0;
  VkDeviceSize bytes = 0;
  for (const Key& key : missing) {
    Image image;
    const asset::ImageData* primary = FindDecoded(jobs, key.primary);
    if (key.kind == TextureKind::kPackedMetallicRoughness) {
      const asset::ImageData* metallic = FindDecoded(jobs, key.secondary);
      if (primary != nullptr || metallic != nullptr) {
        image = UploadTexture(context_,
                              asset::PackMetallicRoughness(primary, metallic),
                              VK_FORMAT_R8G8B8A8_UNORM);
      }
    } else if (primary != nullptr) {
      image = UploadTexture(context_, *primary,
                            key.kind == TextureKind::kColor
                                ? VK_FORMAT_R8G8B8A8_SRGB
                                : VK_FORMAT_R8G8B8A8_UNORM);
    }
    if (image.handle() != VK_NULL_HANDLE) {
      ++loaded;
      bytes += image.size_bytes();
    }
    textures_.emplace(key, std::move(image));
  }
  stats_.count += loaded;
  stats_.bytes += bytes;

  const auto end = std::chrono::steady_clock::now();
  using Milliseconds = std::chrono::duration<double, std::milli>;
  core::log::Info(
      "Loaded {} textures ({:.1f} MiB) in {:.0f} ms (decode {:.0f} ms on {} "
      "jobs, upload {:.0f} ms)",
      loaded, static_cast<double>(bytes) / (1024.0 * 1024.0),
      Milliseconds(end - start).count(), Milliseconds(decoded - start).count(),
      jobs.size(), Milliseconds(end - decoded).count());
}

}  // namespace blue::render
