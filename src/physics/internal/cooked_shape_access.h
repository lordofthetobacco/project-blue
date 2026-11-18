#ifndef BLUE_PHYSICS_INTERNAL_COOKED_SHAPE_ACCESS_H_
#define BLUE_PHYSICS_INTERNAL_COOKED_SHAPE_ACCESS_H_

// Internal to the physics library: this header exposes Jolt types, so it lives
// in internal/ and must not be included from the public physics headers.

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
// clang-format on

#include "physics/shape_cooking.h"

namespace blue::physics {

// The Jolt shape a CookedShape shares between its copies.
struct CookedShape::Holder {
  JPH::ShapeRefC shape;
};

namespace internal {

// Gives physics code (e.g. body creation from prefabs) the Jolt shape of a
// CookedShape.
struct CookedShapeAccess {
  // Returns the restored Jolt shape; never null.
  [[nodiscard]] static const JPH::ShapeRefC& Shape(const CookedShape& shape) {
    return shape.holder_->shape;
  }
};

}  // namespace internal
}  // namespace blue::physics

#endif  // BLUE_PHYSICS_INTERNAL_COOKED_SHAPE_ACCESS_H_
