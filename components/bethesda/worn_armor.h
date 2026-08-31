#ifndef RECREATION_BETHESDA_WORN_ARMOR_H_
#define RECREATION_BETHESDA_WORN_ARMOR_H_

// What an equipped armour actually puts on a body.
//
// An ARMO record does not name the mesh an actor wears. Its own MOD2 is the
// model the thing takes lying on the floor ("...GND.nif"); the worn mesh lives
// on an armature record (ARMA), one per race family, and the armour lists the
// armatures it has in its MODL subrecords. So dressing an actor is a two step
// record walk, ARMO -> ARMA -> model, and which armature applies depends on the
// actor's race.
//
// Verified against Skyrim.esm and its add-ons: DLC1ArmorDawnguardBootsLight
// (0x0200F400) lists one armature, 0x0200F3FF, whose male model is
// DLC01\Armor\Dawnguard\DawnguardBoots1_1.nif, while the armour's own MOD2 is
// the ...GND.nif ground model.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/form_id.h"
#include "core/types.h"

namespace rx::bethesda {

class RecordStore;

// Skyrim numbers the biped slots 30 to 61 and packs them into the first u32 of
// an armour's body template (BOD2, or BODT on a record written before SE), slot
// 30 at bit 0. Confirmed on the reference save's worn set: the Morag Tong
// cuirass writes 0x00000004 (slot 32), the Dawnguard gauntlets 0x00000008
// (slot 33), the boots 0x00000080 (slot 37) and the Bone Hawk amulet
// 0x00000020 (slot 35).
constexpr u32 kBipedSlotFirst = 30;
constexpr u32 kBipedSlotHead = 30;
constexpr u32 kBipedSlotHair = 31;
constexpr u32 kBipedSlotBody = 32;
constexpr u32 kBipedSlotHands = 33;
constexpr u32 kBipedSlotFeet = 37;

constexpr u32 BipedSlotBit(u32 slot) {
  return slot < kBipedSlotFirst ? 0u : 1u << (slot - kBipedSlotFirst);
}

// The one thing an actor's rig needs out of an equipped armour: a skinned NIF
// and the slots it fills, so whatever the armour covers can be taken off the
// bare body.
struct WornArmor {
  GlobalFormId armature;  // the ARMA the model came from
  base::String model;     // skinned NIF path, as the record spells it
  u32 slots = 0;          // biped slot mask, bit 0 = slot 30
};

// Resolves what `armor` puts on an actor of `race`.
//
// The armature is chosen by race: the one whose RNAM is that race, else one
// that lists it among the additional races its own MODL subrecords name, else
// one authored for DefaultRace. `female` picks MOD3 over MOD2, falling back to
// the other when an armature only ships one.
//
// False when the form is not an ARMO, when it lists no armature, or when no
// armature carries a model, so a caller never gets an empty path to load.
bool ResolveWornArmor(const RecordStore& records,
                      GlobalFormId armor,
                      GlobalFormId race,
                      bool female,
                      WornArmor* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_WORN_ARMOR_H_
