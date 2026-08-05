#ifndef RECREATION_WORLD_CARRIAGE_RECORDS_H_
#define RECREATION_WORLD_CARRIAGE_RECORDS_H_

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "core/types.h"

namespace rx::bethesda {
class RecordStore;
}

namespace rx::world {

// How a horse-drawn carriage is really encoded, read back out of the records.
//
// There is no carriage object in Skyrim. One is five placed references wired
// together by linked references (XLKR), most of them keyworded:
//
//   driver (ACHR) --LinkCarriageHorse--> horse (ACHR) --XLKR--> harness (FURN)
//        |  \--LinkCarriageSeat--> seat (FURN, where a passenger rides)
//        \--XLKR--> cart (FURN; its model is the carriage, and its furniture
//                   marker is the driver's bench)
//
// What attaches the horse is furniture: its CarriageSitTarget package sends it
// to its own linked reference, which parks it in the shafts. The cart and seat
// pieces are placed along the same axis, ahead of and behind that mark, so the
// rig's geometry (where the tongue is, where each seat sits) comes out of the
// placements rather than out of any constant here.
struct CarriageRefs {
  u64 driver = 0;   // ACHR of the driver NPC
  u64 horse = 0;    // ACHR of the horse
  u64 cart = 0;     // REFR of the cart body, which is also the driver's seat
  u64 seat = 0;     // REFR of the passenger seat
  u64 harness = 0;  // REFR of the mark the horse stands on, in the shafts

  bool valid() const { return driver && horse && cart; }
};

// The keyword forms that wire a carriage together. Resolved by editor id (a scan
// of the KYWD block), so mods that add carriages are picked up with the vanilla
// ones. `horse` empty means this game does not build carriages this way.
struct CarriageKeywords {
  u64 horse = 0;  // LinkCarriageHorse
  u64 seat = 0;   // LinkCarriageSeat

  bool valid() const { return horse != 0; }
};
CarriageKeywords FindCarriageKeywords(const bethesda::RecordStore& records);

// The carriage `driver_ref` heads, or an invalid one when it heads none. Cheap
// enough to run over the placed actors a cell brings in.
CarriageRefs ResolveCarriage(const bethesda::RecordStore& records,
                             const CarriageKeywords& keywords,
                             u64 driver_ref);

// One journey a carriage horse can be sent on: the game's own travel package
// and the marker chain it walks. Skyrim's carriage network is a package per
// ordered pair of holds (CartHorsePatrolWhiterunToSolitude and friends), all
// stacked on the shared HorseForCarriageNew base, each aimed at a start marker
// that chains onward by XLKR to the destination city.
struct CarriageRoute {
  u64 package = 0;              // packed PACK form id
  base::String destination;     // hold name, from the last named marker
  base::Vector<f32> waypoints;  // x,y,z triples in game units, in travel order
};

// The routes that leave `home` (game units): the horse's travel packages whose
// marker chain starts here and ends somewhere else. Filtering that way is what
// separates the outbound legs from the inbound ones, which are stacked on the
// same base actor and aimed at markers in this very city.
base::Vector<CarriageRoute> ResolveCarriageRoutes(const bethesda::RecordStore& records,
                                                  u64 horse_ref,
                                                  const f32 home[3]);

}  // namespace rx::world

#endif  // RECREATION_WORLD_CARRIAGE_RECORDS_H_
