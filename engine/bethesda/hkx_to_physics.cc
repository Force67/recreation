#include "bethesda/hkx_to_physics.h"

#include <cstring>

namespace rec::bethesda {

physics::ShapeDesc ToShapeDesc(const HkxShape& shape) {
  physics::ShapeDesc desc;
  switch (shape.kind) {
    case HkxShape::Kind::kSphere:
      desc.kind = physics::ShapeDesc::Kind::kSphere;
      desc.radius = shape.radius;
      break;
    case HkxShape::Kind::kCapsule:
      desc.kind = physics::ShapeDesc::Kind::kCapsule;
      desc.radius = shape.radius;
      desc.a = shape.a;
      desc.b = shape.b;
      break;
    case HkxShape::Kind::kBox:
      desc.kind = physics::ShapeDesc::Kind::kBox;
      desc.half_extents = shape.half_extents;
      break;
    case HkxShape::Kind::kConvexVertices:
      desc.kind = physics::ShapeDesc::Kind::kConvexHull;
      desc.vertices = shape.vertices;
      break;
    case HkxShape::Kind::kList:
      desc.kind = physics::ShapeDesc::Kind::kCompound;
      for (const HkxShape& child : shape.children) {
        physics::ShapeDesc converted = ToShapeDesc(child);
        if (converted.kind != physics::ShapeDesc::Kind::kInvalid) {
          desc.children.push_back(std::move(converted));
        }
      }
      if (desc.children.empty()) desc.kind = physics::ShapeDesc::Kind::kInvalid;
      break;
    case HkxShape::Kind::kTransform: {
      desc.kind = physics::ShapeDesc::Kind::kPlaced;
      std::memcpy(desc.transform, shape.transform, sizeof(desc.transform));
      if (!shape.children.empty()) {
        physics::ShapeDesc converted = ToShapeDesc(shape.children[0]);
        if (converted.kind != physics::ShapeDesc::Kind::kInvalid) {
          desc.children.push_back(std::move(converted));
        }
      }
      if (desc.children.empty()) desc.kind = physics::ShapeDesc::Kind::kInvalid;
      break;
    }
    case HkxShape::Kind::kUnknown:
      break;
  }
  return desc;
}

}  // namespace rec::bethesda
