#ifndef RECREATION_QUEST_PACKAGE_RECORD_H_
#define RECREATION_QUEST_PACKAGE_RECORD_H_

#include <base/containers/vector.h>

#include "core/types.h"
#include "components/quest/condition.h"
#include "components/quest/quest_def.h"

namespace rx::bethesda {
struct Record;
class RecordStore;
}  // namespace rx::bethesda

namespace rx::quest {

// Engine-agnostic decode of a PACK (AI Package) record, focused on the one
// thing the runtime needs to drive a scene/escort actor: where the package
// sends it. The Helgen escort (Ralof/Hadvar running to the keep) is built from
// travel packages whose target is a placed marker REFR; MQ101's "friend to
// marker" packages and the dragon-bridge escorts use the same encoding.
//
// PACK layout (Skyrim SE), the parts we read, in declaration order:
//   EDID, [VMAD], PKDT (package header), PSDT (schedule), ...
//   then a typed key/value run of package inputs. Each input is an ANAM type
//   name string ("Location", "SingleRef"/"TargetSelector", "Bool", "Float",
//   ...) followed by the subrecord that carries that type's value:
//     ANAM "Location"       -> PLDT  (location struct, 12 bytes)
//     ANAM "SingleRef"      -> PTDA  (target struct, 12 bytes)
//     ANAM "TargetSelector" -> PTDA
//     ANAM "Bool"/"Float"   -> CNAM  (1 / 4 bytes)
// The first Location/Target input is the package's primary destination, which
// is the one we extract.
//
// PKDT is 12 bytes: u32 flags, u8 type (template procedure index), then bytes.
//
// PLDT (Location) struct: u32 type, u32 data, f32 radius.
//   type 0  Near reference     data = REFR form id
//   type 1  In cell            data = CELL form id
//   type 2  Near self/current  data = 0
//   type 3  Near editor loc    data = 0
//   type 6  Linked reference   data = keyword form id or 0
//   type 8  Reference alias    data = quest alias index
//   type 12 Near package start data = 0
//
// PTDA (Target) struct: i32 type, u32 data, i32 count.
//   type 0  Specific reference data = REFR form id
//   type 1  Object id          data = base form id
//   type 2  Reference alias    data = quest alias index
//   type 3  Linked reference   data = keyword form id or 0
//   type 6  Self               data = 0

// What a package's destination resolves to, independent of the engine. The
// runtime maps this onto a world position: kReference -> the ref's placement
// (DATA pos); kAlias -> resolve through the quest's alias table to a ref, then
// its position; kLinkedRef/kSelf -> relative to the running actor.
struct PackageTarget {
  enum class Kind {
    kNone,       // no target input on the package
    kReference,  // a specific placed reference; `ref` is its packed GlobalFormId
    kAlias,      // a quest alias index; `alias` is the index, resolve at runtime
    kLinkedRef,  // a linked reference off the actor; `ref` is the keyword or 0
    kSelf,       // the actor's own/current location
    kLocation,   // a location/cell or editor point we keep but cannot resolve here
  };

  Kind kind = Kind::kNone;
  u64 ref = 0;       // kReference/kLinkedRef: packed GlobalFormId (or keyword)
  i32 alias = -1;    // kAlias: quest alias index
  f32 radius = 0;    // arrival radius in game units, 0 when unspecified
  u32 raw_kind = 0;  // the PLDT/PTDA type byte as stored, for diagnostics
};

// The decoded shape of one AI package.
struct PackageDef {
  u64 handle = 0;          // packed GlobalFormId of the PACK record
  u16 type = 0;            // PKDT template/procedure type byte
  u32 flags = 0;           // PKDT flags
  bool is_travel = false;  // the package moves the actor to a distinct target
  PackageTarget target;
  // The CTDA gate on this package. An actor runs the highest-priority package
  // whose conditions pass, so this is what decides which leg of a journey is
  // live -- Skyrim's cart-horse patrols gate on the quest's stage. Form ids are
  // resolved when the parse had a record store.
  ConditionList conditions;
};

// Picks the package an actor should be running: the first in `packages` whose
// conditions pass. Bethesda stores an alias's packages highest-priority first,
// so pass them in record order and the winner is the game's own choice. Returns
// -1 when none qualify (the actor falls back to its base AI).
int SelectActivePackage(const base::Vector<PackageDef>& packages, const ConditionContext& ctx);

// Parses one already-decoded PACK record into a PackageDef. `handle` is the
// package's packed form id. `records` resolves the plugin-relative form ids the
// package carries (the target reference) against the load order; pass null to
// leave a reference target as a raw form id. Always succeeds; a package with no
// target input yields PackageTarget::Kind::kNone and never reads past a
// truncated subrecord.
PackageDef ParsePackageRecord(u64 handle, const bethesda::Record& record,
                              const bethesda::RecordStore& records);

// Overload for callers without a store (synthetic data / unit tests): leaves a
// reference target as its raw plugin-relative form id.
PackageDef ParsePackageRecord(u64 handle, const bethesda::Record& record);

// One leg of a route: walk to `marker`, then the package's fragment runs (which
// is how the game advances the quest on arrival).
struct RouteStop {
  u64 package = 0;              // packed PACK form id
  u64 marker = 0;               // packed target REFR form id
  f32 position[3] = {0, 0, 0};  // the marker's placement, game units
};

// The route an alias's AI packages send its actor along: its ALPC list filtered
// to travel packages that name a concrete reference, in the order they run.
// Bethesda stores alias packages highest-priority first and gates each on quest
// stage, so the earliest leg is last in the list and this returns them reversed.
//
// This is how a quest encodes a journey without scripting any motion: MQ101's
// cart horse walks five patrol packages in turn, each package's fragment setting
// the stage that lets the next one take over.
base::Vector<RouteStop> ResolveAliasTravelRoute(const bethesda::RecordStore& records,
                                                const AliasDef& alias, u16 quest_plugin);

}  // namespace rx::quest

#endif  // RECREATION_QUEST_PACKAGE_RECORD_H_
