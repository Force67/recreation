#ifndef RECREATION_BETHESDA_HKX_TO_PHYSICS_H_
#define RECREATION_BETHESDA_HKX_TO_PHYSICS_H_

// Bridges decoded Havok shapes into the engine-neutral physics::ShapeDesc
// tree PhysicsWorld lowers to Jolt. Values stay in the source's units (game
// units for Skyrim data); the world-add calls take the unit scale.

#include "bethesda/hkx_physics.h"
#include "physics/shape_desc.h"

namespace rx::bethesda {

physics::ShapeDesc ToShapeDesc(const HkxShape& shape);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_HKX_TO_PHYSICS_H_
