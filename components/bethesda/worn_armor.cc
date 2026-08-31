#include "components/bethesda/worn_armor.h"

#include <cstring>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"

namespace rx::bethesda {
namespace {

constexpr u32 kArmo = FourCc('A', 'R', 'M', 'O');
constexpr u32 kArma = FourCc('A', 'R', 'M', 'A');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kMod2 = FourCc('M', 'O', 'D', '2');
constexpr u32 kMod3 = FourCc('M', 'O', 'D', '3');
constexpr u32 kBod2 = FourCc('B', 'O', 'D', '2');
constexpr u32 kBodt = FourCc('B', 'O', 'D', 'T');
constexpr u32 kRnam = FourCc('R', 'N', 'A', 'M');

// Every Skyrim armature that is not race specific is authored against this one.
constexpr u32 kDefaultRaceFormId = 0x00000019;

base::String SubString(const Subrecord& sub) {
  if (sub.data.empty())
    return {};
  size_t len = sub.data.size();
  if (sub.data[len - 1] == 0)
    --len;
  return base::String(reinterpret_cast<const char*>(sub.data.data()), len);
}

u32 ReadU32(const Subrecord& sub) {
  u32 value = 0;
  if (sub.data.size() >= 4)
    std::memcpy(&value, sub.data.data(), sizeof(value));
  return value;
}

// The MODL subrecords of a record, in order: on an ARMO the armatures it has,
// on an ARMA the additional races it also fits.
void CollectRefs(const RecordStore& records,
                 const Record& rec,
                 u32 fourcc,
                 u16 plugin,
                 base::Vector<GlobalFormId>* out) {
  for (const Subrecord& sub : rec.subrecords) {
    if (sub.type != fourcc || sub.data.size() < 4)
      continue;
    const u32 raw = ReadU32(sub);
    if (raw == 0)
      continue;
    out->push_back(records.ResolveFrom(RawFormId{raw}, plugin));
  }
}

bool Same(GlobalFormId a, GlobalFormId b) {
  return a.plugin == b.plugin && a.local_id == b.local_id;
}

// How well an armature fits a race. A higher score wins, 0 means it does not
// fit at all and is only used when nothing else does.
u32 RaceFit(const RecordStore& records, const Record& arma, u16 plugin, GlobalFormId race) {
  const Subrecord* rnam = arma.Find(kRnam);
  if (rnam && rnam->data.size() >= 4) {
    const GlobalFormId primary = records.ResolveFrom(RawFormId{ReadU32(*rnam)}, plugin);
    if (Same(primary, race))
      return 3;
    if (primary.plugin == 0 && primary.local_id == kDefaultRaceFormId)
      return 1;
  }
  base::Vector<GlobalFormId> extra;
  CollectRefs(records, arma, kModl, plugin, &extra);
  for (GlobalFormId other : extra) {
    if (Same(other, race))
      return 2;
  }
  return 0;
}

}  // namespace

bool ResolveWornArmor(const RecordStore& records,
                      GlobalFormId armor,
                      GlobalFormId race,
                      bool female,
                      WornArmor* out) {
  const RecordStore::StoredRecord* stored = records.Find(armor);
  if (!stored || stored->header.type != kArmo)
    return false;
  Record rec;
  if (!records.Parse(armor, &rec))
    return false;

  u32 slots = 0;
  const Subrecord* body = rec.Find(kBod2);
  if (!body)
    body = rec.Find(kBodt);
  if (body)
    slots = ReadU32(*body);

  base::Vector<GlobalFormId> armatures;
  CollectRefs(records, rec, kModl, stored->winning_plugin, &armatures);

  GlobalFormId best;
  base::String best_model;
  u32 best_fit = 0;
  bool found = false;
  for (GlobalFormId id : armatures) {
    const RecordStore::StoredRecord* arma_stored = records.Find(id);
    if (!arma_stored || arma_stored->header.type != kArma)
      continue;
    Record arma;
    if (!records.Parse(id, &arma))
      continue;
    const Subrecord* first = arma.Find(female ? kMod3 : kMod2);
    if (!first)
      first = arma.Find(female ? kMod2 : kMod3);
    if (!first)
      continue;  // an armature with no model dresses nobody
    const u32 fit = RaceFit(records, arma, arma_stored->winning_plugin, race);
    // A first find wins over nothing at all, so an armour whose armatures are
    // all authored for other races still renders rather than vanishing.
    if (found && fit <= best_fit)
      continue;
    best = id;
    best_model = SubString(*first);
    best_fit = fit;
    found = true;
    if (fit == 3)
      break;
  }
  if (!found || best_model.empty())
    return false;

  out->armature = best;
  out->model = base::move(best_model);
  out->slots = slots;
  return true;
}

}  // namespace rx::bethesda
