#ifndef RECREATION_BETHESDA_MOVEMENT_TYPE_H_
#define RECREATION_BETHESDA_MOVEMENT_TYPE_H_

#include <string>
#include <unordered_map>

#include "bethesda/load_order.h"
#include "core/types.h"

// MOVT (movement type) decode. Each MOVT record carries a SPED subrecord with
// the directional walk/run linear speeds and the rotation speeds a movement
// type applies. Skyrim's default humanoid actor (and the player) use the
// "NPC_Default_MT" movement type, so its forward walk/run speeds are the
// authoritative source for player locomotion tuning (see runtime/player_controller).
//
// All linear speeds are in Bethesda game units per second; multiply by the
// game profile's units_to_meters (~0.01428) to get engine m/s. Rotation speeds
// are degrees per second. This mirrors the weather_loader / planet decode
// conventions: a free LoadMovementTypes over the RecordStore, no dispatch table.
namespace rx::bethesda {

// One decoded MOVT record. Field names follow the SPED subrecord layout
// (11 float32, 44 bytes): the four cardinal directions each with a walk and a
// run speed, then the rotation speeds. Missing/short SPED leaves fields at 0.
struct MovementType {
  u64 form = 0;
  std::string editor_id;

  f32 left_walk = 0, left_run = 0;
  f32 right_walk = 0, right_run = 0;
  f32 forward_walk = 0, forward_run = 0;
  f32 back_walk = 0, back_run = 0;
  f32 rotate_in_place_walk = 0;  // deg/s
  f32 rotate_in_place_run = 0;   // deg/s
  f32 rotate_while_moving_run = 0;  // deg/s

  bool has_speeds = false;  // a SPED subrecord was present and decoded
};

// Decodes every MOVT record into `out` keyed by packed GlobalFormId. Returns the
// number decoded. Cheap; run once after game data loads.
int LoadMovementTypes(const RecordStore& records, std::unordered_map<u64, MovementType>* out);

// Finds a movement type by editor id (e.g. "NPC_Default_MT"). Null if absent.
const MovementType* FindMovementType(const std::unordered_map<u64, MovementType>& types,
                                     std::string_view editor_id);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_MOVEMENT_TYPE_H_
