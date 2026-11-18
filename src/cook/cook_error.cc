#include "cook/cook_error.h"

namespace blue::cook {

const char* ToString(CookError error) {
  switch (error) {
    case CookError::kFileNotFound:
      return "file not found";
    case CookError::kParseFailed:
      return "parse failed";
    case CookError::kNoMeshes:
      return "no triangle meshes";
    case CookError::kTooLarge:
      return "too large";
    case CookError::kInvalidImage:
      return "invalid image";
    case CookError::kTextureLoadFailed:
      return "texture load failed";
    case CookError::kInvalidArgument:
      return "invalid argument";
    case CookError::kShapeCookFailed:
      return "collision shape cooking failed";
  }
  return "unknown";
}

}  // namespace blue::cook
