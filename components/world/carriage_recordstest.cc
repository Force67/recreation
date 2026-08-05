// carriage_recordstest: the carriage graph is read back out of a plugin this
// test authors, so the link walking is checked without game data. Builds the
// shape Skyrim uses (a driver linking its horse and passenger seat by keyword,
// its cart by a bare link, and the horse linking the mark it stands on) plus a
// horse base carrying travel packages out of town and back, and checks that
// ResolveCarriage finds the whole rig and ResolveCarriageRoutes offers only the
// legs that leave.

#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "components/bethesda/game_profile.h"
#include "components/bethesda/load_order.h"
#include "components/bethesda/writer.h"
#include "components/world/carriage_records.h"
#include "core/types.h"

using namespace rx;
using namespace rx::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

constexpr u32 kAchr = FourCc('A', 'C', 'H', 'R');
constexpr u32 kRefr = FourCc('R', 'E', 'F', 'R');
constexpr u32 kKywd = FourCc('K', 'Y', 'W', 'D');
constexpr u32 kNpc = FourCc('N', 'P', 'C', '_');
constexpr u32 kPack = FourCc('P', 'A', 'C', 'K');
constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kXlkr = FourCc('X', 'L', 'K', 'R');
constexpr u32 kPkid = FourCc('P', 'K', 'I', 'D');
constexpr u32 kPkdt = FourCc('P', 'K', 'D', 'T');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kPtda = FourCc('P', 'T', 'D', 'A');

// Form ids, in the one plugin this test writes.
constexpr u32 kLinkHorse = 0x800;
constexpr u32 kLinkSeat = 0x801;
constexpr u32 kDriverRef = 0x810;
constexpr u32 kHorseRef = 0x811;
constexpr u32 kCartRef = 0x812;
constexpr u32 kSeatRef = 0x813;
constexpr u32 kHarnessRef = 0x814;
constexpr u32 kHorseBase = 0x820;
constexpr u32 kOutbound = 0x830;
constexpr u32 kInbound = 0x831;
constexpr u32 kFarMark = 0x840;   // the outbound leg's first mark, just out of town
constexpr u32 kFarMark2 = 0x841;  // where it ends, a hold away
constexpr u32 kHomeMark = 0x842;  // where the inbound leg ends, here

GameProfile TestProfile() {
  GameProfile profile;
  profile.game = Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  return profile;
}

// REFR/ACHR DATA: position xyz then rotation xyz, all f32 game units.
base::Vector<u8> Placement(f32 x, f32 y, f32 z) {
  base::Vector<u8> d(24, 0);
  std::memcpy(d.data() + 0, &x, 4);
  std::memcpy(d.data() + 4, &y, 4);
  std::memcpy(d.data() + 8, &z, 4);
  return d;
}

// XLKR: keyword form id then the linked reference (0 keyword = a bare link).
base::Vector<u8> Link(u32 keyword, u32 ref) {
  base::Vector<u8> d(8);
  std::memcpy(d.data() + 0, &keyword, 4);
  std::memcpy(d.data() + 4, &ref, 4);
  return d;
}

// PTDA: a "specific reference" target (type 0) at `ref`.
base::Vector<u8> RefTarget(u32 ref) {
  base::Vector<u8> d(12, 0);
  const i32 type = 0;
  std::memcpy(d.data() + 0, &type, 4);
  std::memcpy(d.data() + 4, &ref, 4);
  return d;
}

void AddRef(PluginWriter& plugin, u32 type, u32 id, const char* editor_id) {
  RecordBuilder record(type, RawFormId{id});
  if (editor_id[0])
    record.EditorId(editor_id);
  plugin.AddRecord(record.record());
}

void WritePlugin(const GameProfile& profile, const base::String& path) {
  PluginWriter plugin(profile);
  plugin.set_author("carriage").set_master(true);

  AddRef(plugin, kKywd, kLinkHorse, "LinkCarriageHorse");
  AddRef(plugin, kKywd, kLinkSeat, "LinkCarriageSeat");

  // The driver: horse and seat by keyword, the cart body on a bare link.
  {
    RecordBuilder driver(kAchr, RawFormId{kDriverRef});
    driver.EditorId("TestCarriageDriverRef");
    base::Vector<u8> data = Placement(1000, 1000, 0);
    driver.Field(kData, ByteSpan(data.data(), data.size()));
    base::Vector<u8> cart = Link(0, kCartRef);
    driver.Field(kXlkr, ByteSpan(cart.data(), cart.size()));
    base::Vector<u8> horse = Link(kLinkHorse, kHorseRef);
    driver.Field(kXlkr, ByteSpan(horse.data(), horse.size()));
    base::Vector<u8> seat = Link(kLinkSeat, kSeatRef);
    driver.Field(kXlkr, ByteSpan(seat.data(), seat.size()));
    plugin.AddRecord(driver.record());
  }

  // The horse: its base carries the travel packages, its bare link is the mark
  // it stands on.
  {
    RecordBuilder horse(kAchr, RawFormId{kHorseRef});
    horse.EditorId("TestCarriageHorseRef");
    horse.FieldPod(kName, kHorseBase);
    base::Vector<u8> data = Placement(1100, 1000, 0);
    horse.Field(kData, ByteSpan(data.data(), data.size()));
    base::Vector<u8> harness = Link(0, kHarnessRef);
    horse.Field(kXlkr, ByteSpan(harness.data(), harness.size()));
    plugin.AddRecord(horse.record());
  }

  for (const auto& [id, name, x, y] :
       {std::tuple{kCartRef, "", 1000.0f, 1050.0f}, std::tuple{kSeatRef, "", 1000.0f, 1100.0f},
        std::tuple{kHarnessRef, "", 1100.0f, 1000.0f},
        std::tuple{kFarMark, "TestTownCartPatrolStartFar", 1400.0f, 1000.0f},
        std::tuple{kFarMark2, "FarholdCarriageDestinationMarker", 90000.0f, 1000.0f},
        std::tuple{kHomeMark, "TestTownCarriageDestinationMarker", 1050.0f, 1000.0f}}) {
    RecordBuilder record(kRefr, RawFormId{id});
    if (name[0])
      record.EditorId(name);
    base::Vector<u8> data = Placement(x, y, 0);
    record.Field(kData, ByteSpan(data.data(), data.size()));
    if (id == kFarMark) {
      base::Vector<u8> next = Link(0, kFarMark2);  // the chain on to the far hold
      record.Field(kXlkr, ByteSpan(next.data(), next.size()));
    }
    plugin.AddRecord(record.record());
  }

  {
    RecordBuilder base(kNpc, RawFormId{kHorseBase});
    base.EditorId("TestCarriageHorse");
    base.FieldPod(kPkid, kOutbound);
    base.FieldPod(kPkid, kInbound);
    plugin.AddRecord(base.record());
  }

  // Two travel packages: one out to the far hold, one arriving back here. Both
  // are stacked on the shared base, which is why the routes have to be filtered
  // by where they lead rather than by which actor carries them.
  for (const auto& [id, name, target] : {std::tuple{kOutbound, "CartHorsePatrolTestToFar", kFarMark},
                                         std::tuple{kInbound, "CartHorsePatrolFarToTest",
                                                    kHomeMark}}) {
    RecordBuilder pack(kPack, RawFormId{id});
    pack.EditorId(name);
    base::Vector<u8> header(12, 0);
    pack.Field(kPkdt, ByteSpan(header.data(), header.size()));
    base::Vector<u8> kind(10);
    std::memcpy(kind.data(), "SingleRef", 10);
    pack.Field(kAnam, ByteSpan(kind.data(), kind.size()));
    base::Vector<u8> target_bytes = RefTarget(target);
    pack.Field(kPtda, ByteSpan(target_bytes.data(), target_bytes.size()));
    plugin.AddRecord(pack.record());
  }

  plugin.Save(path);
}

}  // namespace

int main() {
  const base::String dir = std::filesystem::temp_directory_path().string();
  const base::String path = dir + "/CarriageTest.esm";
  const GameProfile profile = TestProfile();
  WritePlugin(profile, path);

  LoadOrder order;
  order.Append("CarriageTest.esm");
  RecordStore store;
  Check("load the authored plugin", store.LoadAll(dir, order, profile));

  std::puts("carriage keywords:");
  const world::CarriageKeywords keywords = world::FindCarriageKeywords(store);
  Check("LinkCarriageHorse found", keywords.valid());
  Check("LinkCarriageSeat found", keywords.seat != 0);

  std::puts("carriage graph:");
  const world::CarriageRefs refs =
      world::ResolveCarriage(store, keywords, GlobalFormId{0, kDriverRef}.packed());
  Check("driver heads a carriage", refs.valid());
  Check("horse resolved", refs.horse == GlobalFormId{0, kHorseRef}.packed());
  Check("cart resolved from the bare link", refs.cart == GlobalFormId{0, kCartRef}.packed());
  Check("seat resolved by keyword", refs.seat == GlobalFormId{0, kSeatRef}.packed());
  Check("harness resolved off the horse", refs.harness == GlobalFormId{0, kHarnessRef}.packed());

  const world::CarriageRefs none =
      world::ResolveCarriage(store, keywords, GlobalFormId{0, kHorseRef}.packed());
  Check("an actor with no horse link heads none", !none.valid());

  std::puts("routes out of town:");
  const f32 home[3] = {1000, 1000, 0};
  const base::Vector<world::CarriageRoute> routes =
      world::ResolveCarriageRoutes(store, GlobalFormId{0, kHorseRef}.packed(), home);
  Check("one route offered", routes.size() == 1);
  if (routes.size() == 1) {
    Check("it is the outbound leg", routes[0].package == GlobalFormId{0, kOutbound}.packed());
    Check("named for where it ends", routes[0].destination == "Farhold");
    Check("both marks in travel order", routes[0].waypoints.size() == 6);
    Check("starting here", routes[0].waypoints[0] == 1400.0f);
  }

  std::printf("%s\n", g_failures ? "FAILED" : "all ok");
  return g_failures ? 1 : 0;
}
