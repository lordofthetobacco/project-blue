#ifndef BLUE_COOK_COOK_ERROR_H_
#define BLUE_COOK_COOK_ERROR_H_

namespace blue::cook {

// Why cooking failed. Details (paths, names) are logged where they happen.
enum class CookError {
  kFileNotFound,       // A source file does not exist.
  kParseFailed,        // The model importer could not parse the file.
  kNoMeshes,           // The model holds no usable triangle meshes.
  kTooLarge,           // A mesh or image exceeds the supported size.
  kInvalidImage,       // An image is empty or has inconsistent dimensions.
  kTextureLoadFailed,  // A referenced texture could not be read or decoded.
  kInvalidArgument,    // An option, name, index or time range is invalid.
  kShapeCookFailed,    // The physics library rejected a collision shape.
};

// Returns a short human-readable name for `error`. Never null.
[[nodiscard]] const char* ToString(CookError error);

}  // namespace blue::cook

#endif  // BLUE_COOK_COOK_ERROR_H_
