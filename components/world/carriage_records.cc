#include "components/world/carriage_records.h"

#include <base/memory/move.h>

#include <cmath>
#include <cstring>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"
#include "components/quest/package_record.h"

namespace rx::world {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kXlkr = FourCc('X', 'L', 'K', 'R');
constexpr u32 kPkid = FourCc('P', 'K', 'I', 'D');
constexpr u32 kKywd = FourCc('K', 'Y', 'W', 'D');

// A leg only leaves town if it ends this far from where it started, and only
// starts here if its first mark is this close (game units; a hold capital is a
// couple of thousand across and the shortest carriage run is tens of thousands).
constexpr f32 kOutboundDistance = 20000.0f;
constexpr f32 kHomeDistance = 4000.0f;

bethesda::GlobalFormId Unpack(u64 handle) {
  return {static_cast<u16>(handle >> 32), static_cast<u32>(handle)};
}

u32 RawAt(const bethesda::Subrecord& sub, size_t offset) {
  if (sub.data.size() < offset + 4)
    return 0;
  u32 raw = 0;
  std::memcpy(&raw, sub.data.data() + offset, 4);
  return raw;
}

// The reference `ref` links to under `keyword` (0 = the first unkeyworded link).
// A reference carries one XLKR subrecord per link, so this walks them all.
u64 LinkedRef(const bethesda::RecordStore& records,
              bethesda::GlobalFormId ref,
              const bethesda::Record& record,
              u64 keyword) {
  const bethesda::RecordStore::StoredRecord* stored = records.Find(ref);
  const u16 plugin = stored ? stored->winning_plugin : 0;
  for (const bethesda::Subrecord& sub : record.subrecords) {
    if (sub.type != kXlkr || sub.data.size() < 8)
      continue;
    const u32 keyword_raw = RawAt(sub, 0);
    const u64 linked_keyword =
        keyword_raw ? records.ResolveFrom(bethesda::RawFormId{keyword_raw}, plugin).packed() : 0;
    if (linked_keyword != keyword)
      continue;
    const u32 target = RawAt(sub, 4);
    if (!target)
      continue;
    return records.ResolveFrom(bethesda::RawFormId{target}, plugin).packed();
  }
  return 0;
}

bool RefPosition(const bethesda::Record& record, f32 out[3]) {
  const bethesda::Subrecord* data = record.Find(kData);
  if (!data || data->data.size() < 12)
    return false;
  std::memcpy(out, data->data.data(), 12);
  return true;
}

f32 PlanarDistance(const f32 a[3], const f32 b[3]) {
  const f32 dx = a[0] - b[0], dy = a[1] - b[1];
  return std::sqrt(dx * dx + dy * dy);
}

// "SolitudeCarriageDestinationMarker" -> "Solitude". The city is the part of a
// marker's editor id before the word the level designers tagged it with.
base::String HoldName(const base::String& editor_id) {
  for (const char* tag : {"Carriage", "CartPatrol", "Cart"}) {
    const size_t at = editor_id.find(tag);
    if (at != base::String::npos && at > 0)
      return editor_id.substr(0, at);
  }
  return editor_id;
}

}  // namespace

CarriageKeywords FindCarriageKeywords(const bethesda::RecordStore& records) {
  CarriageKeywords out;
  records.EachOfType(
      kKywd, [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
        if (out.horse && out.seat)
          return;
        bethesda::Record record;
        if (!records.Parse(id, &record))
          return;
        const base::String editor_id = record.GetString(kEdid);
        if (editor_id == "LinkCarriageHorse")
          out.horse = id.packed();
        else if (editor_id == "LinkCarriageSeat")
          out.seat = id.packed();
      });
  return out;
}

CarriageRefs ResolveCarriage(const bethesda::RecordStore& records,
                             const CarriageKeywords& keywords,
                             u64 driver_ref) {
  CarriageRefs refs;
  if (!keywords.valid())
    return refs;
  const bethesda::GlobalFormId driver_id = Unpack(driver_ref);
  bethesda::Record driver;
  if (!records.Parse(driver_id, &driver))
    return refs;
  const u64 horse = LinkedRef(records, driver_id, driver, keywords.horse);
  if (!horse)
    return refs;  // not a carriage driver

  refs.driver = driver_ref;
  refs.horse = horse;
  refs.cart = LinkedRef(records, driver_id, driver, 0);
  refs.seat = keywords.seat ? LinkedRef(records, driver_id, driver, keywords.seat) : 0;
  bethesda::Record horse_record;
  if (records.Parse(Unpack(horse), &horse_record))
    refs.harness = LinkedRef(records, Unpack(horse), horse_record, 0);
  return refs;
}

base::Vector<CarriageRoute> ResolveCarriageRoutes(const bethesda::RecordStore& records,
                                                  u64 horse_ref,
                                                  const f32 home[3]) {
  base::Vector<CarriageRoute> out;
  bethesda::Record horse;
  if (!records.Parse(Unpack(horse_ref), &horse))
    return out;
  const bethesda::RecordStore::StoredRecord* horse_stored = records.Find(Unpack(horse_ref));
  const bethesda::Subrecord* name = horse.Find(kName);
  if (!name || name->data.size() < 4)
    return out;
  const bethesda::GlobalFormId base_npc = records.ResolveFrom(
      bethesda::RawFormId{RawAt(*name, 0)}, horse_stored ? horse_stored->winning_plugin : 0);

  bethesda::Record npc;
  if (!records.Parse(base_npc, &npc))
    return out;
  const bethesda::RecordStore::StoredRecord* npc_stored = records.Find(base_npc);
  const u16 npc_plugin = npc_stored ? npc_stored->winning_plugin : 0;

  for (const bethesda::Subrecord& sub : npc.subrecords) {
    if (sub.type != kPkid || sub.data.size() < 4)
      continue;
    const bethesda::GlobalFormId pack_id =
        records.ResolveFrom(bethesda::RawFormId{RawAt(sub, 0)}, npc_plugin);
    bethesda::Record pack;
    if (!records.Parse(pack_id, &pack))
      continue;
    const quest::PackageDef def = quest::ParsePackageRecord(pack_id.packed(), pack, records);
    if (!def.is_travel || def.target.kind != quest::PackageTarget::Kind::kReference ||
        !def.target.ref)
      continue;

    // Follow the marker the package names, then the chain of unnamed markers it
    // heads: that chain is the road this carriage takes.
    CarriageRoute route;
    route.package = pack_id.packed();
    base::String last_named;
    u64 marker = def.target.ref;
    for (int hop = 0; hop < 64 && marker; ++hop) {
      bethesda::Record record;
      f32 position[3];
      if (!records.Parse(Unpack(marker), &record) || !RefPosition(record, position))
        break;
      route.waypoints.push_back(position[0]);
      route.waypoints.push_back(position[1]);
      route.waypoints.push_back(position[2]);
      const base::String editor_id = record.GetString(kEdid);
      if (!editor_id.empty())
        last_named = editor_id;
      marker = LinkedRef(records, Unpack(marker), record, 0);
    }
    if (route.waypoints.size() < 6)
      continue;  // a single mark is a place to stand, not a journey

    const f32* first = route.waypoints.data();
    const f32* last = route.waypoints.data() + route.waypoints.size() - 3;
    if (PlanarDistance(first, home) > kHomeDistance)
      continue;  // some other city's leg, stacked on the same shared base actor
    if (PlanarDistance(last, home) < kOutboundDistance)
      continue;  // a leg arriving here, not leaving

    route.destination = HoldName(last_named);
    out.push_back(base::move(route));
  }
  return out;
}

}  // namespace rx::world
