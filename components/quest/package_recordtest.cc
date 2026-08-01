// package_recordtest: checks the PACK travel-target parser. The deterministic
// part builds synthetic PACK records (one per target encoding) and runs in the
// ctest gate (no game data). With
//
//   package_recordtest <data_dir>
//
// it additionally loads the real record store and dumps Skyrim's travel/escort
// packages (the MQ101 Helgen run-to-keep markers among them), which is how the
// PLDT/PTDA layout was validated.

#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/strings/xstring.h>

#include <cstdio>
#include <cstring>

#include "components/bethesda/load_order.h"
#include "components/bethesda/record.h"
#include "components/quest/package_record.h"
#include "core/types.h"

using namespace rx;
// rx::u64/i64 (long) and base/arch.h's (long long) are different types sharing
// a global name, so the 64-bit spellings below are qualified; the other scalars
// agree between the two and need no help.
using namespace rx::quest;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

// Backing store for the synthetic record's subrecord spans.
struct Buffers {
  base::Vector<base::Vector<u8>> store;
  ByteSpan Bytes(const void* p, size_t n) {
    auto& b = store.emplace_back(n);
    if (n)
      std::memcpy(b.data(), p, n);
    return ByteSpan(b.data(), b.size());
  }
  ByteSpan Empty() { return Bytes(nullptr, 0); }
  ByteSpan Str(const char* s) { return Bytes(s, std::strlen(s) + 1); }
  ByteSpan U32(u32 v) { return Bytes(&v, sizeof(v)); }
  ByteSpan U8(u8 v) { return Bytes(&v, sizeof(v)); }
  // A 12-byte PKDT: flags u32, type u8, then padding.
  ByteSpan Pkdt(u32 flags, u8 type) {
    u8 b[12] = {0};
    std::memcpy(b, &flags, 4);
    b[4] = type;
    return Bytes(b, sizeof(b));
  }
  // A 12-byte PLDT (Location): type u32, data u32, radius f32.
  ByteSpan Pldt(u32 type, u32 data, f32 radius) {
    u8 b[12];
    std::memcpy(b, &type, 4);
    std::memcpy(b + 4, &data, 4);
    std::memcpy(b + 8, &radius, 4);
    return Bytes(b, sizeof(b));
  }
  // A 12-byte PTDA (Target): type i32, data u32, count i32.
  ByteSpan Ptda(i32 type, u32 data, i32 count) {
    u8 b[12];
    std::memcpy(b, &type, 4);
    std::memcpy(b + 4, &data, 4);
    std::memcpy(b + 8, &count, 4);
    return Bytes(b, sizeof(b));
  }
};

void Add(bethesda::Record& r, u32 type, ByteSpan data) {
  bethesda::Subrecord sub;
  sub.type = type;
  sub.data = data;
  r.subrecords.push_back(base::move(sub));
}

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kPkdt = FourCc('P', 'K', 'D', 'T');
constexpr u32 kAnam = FourCc('A', 'N', 'A', 'M');
constexpr u32 kPldt = FourCc('P', 'L', 'D', 'T');
constexpr u32 kPtda = FourCc('P', 'T', 'D', 'A');
constexpr u32 kCnam = FourCc('C', 'N', 'A', 'M');

// MQ101RalofRuntoKeepOutsideMarker shape: header, then the first input is a
// "Location" type pointing Near a marker REFR. A trailing Bool input is added
// to prove the parser stops at the first target.
bethesda::Record MakeRefTravel(Buffers& buf) {
  bethesda::Record r;
  Add(r, kEdid, buf.Str("MQ101RalofRuntoKeepOutsideMarker"));
  Add(r, kPkdt, buf.Pkdt(0x08902000, 18));
  Add(r, kAnam, buf.Str("Location"));
  Add(r, kPldt, buf.Pldt(0, 0x0107D8, 0.0f));  // type 0 = near reference
  Add(r, kAnam, buf.Str("Bool"));
  Add(r, kCnam, buf.U8(1));
  // A second, secondary location must be ignored.
  Add(r, kAnam, buf.Str("Location"));
  Add(r, kPldt, buf.Pldt(0, 0xDEAD, 99.0f));
  return r;
}

// DA03BarbasEscortToShrine shape: a "SingleRef" input whose PTDA selects a
// reference alias (the alias the escort follows).
bethesda::Record MakeAliasTravel(Buffers& buf) {
  bethesda::Record r;
  Add(r, kEdid, buf.Str("EscortToAlias"));
  Add(r, kPkdt, buf.Pkdt(0x00002000, 18));
  Add(r, kAnam, buf.Str("SingleRef"));
  Add(r, kPtda, buf.Ptda(2, 20, 0));  // type 2 = reference alias, index 20
  return r;
}

// WERoad12Travel shape: Near reference with a non-zero arrival radius.
bethesda::Record MakeRadiusTravel(Buffers& buf) {
  bethesda::Record r;
  Add(r, kEdid, buf.Str("WERoad12Travel"));
  Add(r, kPkdt, buf.Pkdt(0x08102000, 18));
  Add(r, kAnam, buf.Str("Location"));
  Add(r, kPldt, buf.Pldt(0, 0x10C111, 512.0f));
  return r;
}

// HoldPosition shape: PLDT type 2 = near current location (the actor stays).
bethesda::Record MakeHoldPosition(Buffers& buf) {
  bethesda::Record r;
  Add(r, kEdid, buf.Str("MQ101HoldCurrentPositionWeapons"));
  Add(r, kPkdt, buf.Pkdt(0x08101010, 18));
  Add(r, kAnam, buf.Str("Location"));
  Add(r, kPldt, buf.Pldt(2, 0, 0.0f));
  return r;
}

// PLDT type 8 = reference alias (the location-form of an alias target).
bethesda::Record MakeLocationAlias(Buffers& buf) {
  bethesda::Record r;
  Add(r, kEdid, buf.Str("FriendToKeepAlias"));
  Add(r, kPkdt, buf.Pkdt(0x08002000, 18));
  Add(r, kAnam, buf.Str("Location"));
  Add(r, kPldt, buf.Pldt(8, 173, 101.0f));  // type 8 = reference alias index 173
  return r;
}

void TestRefTravel() {
  std::puts("package: near-reference travel (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeRefTravel(buf);
  PackageDef def = ParsePackageRecord(0xabc123ull, r);  // raw form ids
  Check("handle preserved", def.handle == 0xabc123ull);
  Check("pkdt flags", def.flags == 0x08902000);
  Check("pkdt type", def.type == 18);
  Check("kind reference", def.target.kind == PackageTarget::Kind::kReference);
  Check("ref raw form id", def.target.ref == 0x0107D8);
  Check("alias unset", def.target.alias == -1);
  Check("radius zero", def.target.radius == 0.0f);
  Check("is travel", def.is_travel);
  Check("stopped at first target (not 0xDEAD)", def.target.ref != 0xDEAD);
}

void TestAliasTravel() {
  std::puts("package: reference-alias travel (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeAliasTravel(buf);
  PackageDef def = ParsePackageRecord(0x1ull, r);
  Check("kind alias", def.target.kind == PackageTarget::Kind::kAlias);
  Check("alias index 20", def.target.alias == 20);
  Check("ref unset", def.target.ref == 0);
  Check("is travel", def.is_travel);
}

void TestRadiusTravel() {
  std::puts("package: travel with arrival radius (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeRadiusTravel(buf);
  PackageDef def = ParsePackageRecord(0x2ull, r);
  Check("kind reference", def.target.kind == PackageTarget::Kind::kReference);
  Check("ref raw form id", def.target.ref == 0x10C111);
  Check("radius 512", def.target.radius == 512.0f);
  Check("is travel", def.is_travel);
}

void TestHoldPosition() {
  std::puts("package: hold-position is not travel (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeHoldPosition(buf);
  PackageDef def = ParsePackageRecord(0x3ull, r);
  Check("kind self", def.target.kind == PackageTarget::Kind::kSelf);
  Check("not travel", !def.is_travel);
}

void TestLocationAlias() {
  std::puts("package: location-form alias (synthetic):");
  Buffers buf;
  bethesda::Record r = MakeLocationAlias(buf);
  PackageDef def = ParsePackageRecord(0x4ull, r);
  Check("kind alias", def.target.kind == PackageTarget::Kind::kAlias);
  Check("alias index 173", def.target.alias == 173);
  Check("radius 101", def.target.radius == 101.0f);
  Check("is travel", def.is_travel);
}

// A context that answers GetStage for one quest, which is what a journey's
// packages gate on.
class StageContext : public ConditionContext {
 public:
  explicit StageContext(float stage) : stage_(stage) {}
  float GetStage(rx::u64) const override { return stage_; }

 private:
  float stage_;
};

// An actor runs the highest-priority package whose conditions pass. Alias
// packages are stored highest-priority first, so a journey's legs are listed
// last-to-first and the selector walks them in that order.
void TestSelectActivePackage() {
  auto leg = [](float at_least_stage) {
    PackageDef def;
    Comparison c;
    c.func = Func::kGetStage;
    c.param1 = 0x1234u;
    c.op = CompareOp::kGreaterOrEqual;
    c.value = at_least_stage;
    def.conditions.comparisons.push_back(c);
    return def;
  };
  // Listed highest priority first: the last leg outranks the earlier ones.
  base::Vector<PackageDef> packages = {leg(40), leg(30), leg(20), leg(10)};

  StageContext before(0);
  Check("no package qualifies before the quest starts",
        SelectActivePackage(packages, before) == -1);

  StageContext early(10);
  Check("the first leg runs at its stage", SelectActivePackage(packages, early) == 3);

  StageContext mid(30);
  Check("a later stage promotes the higher-priority leg", SelectActivePackage(packages, mid) == 1);

  StageContext late(99);
  Check("the last leg wins once every gate passes", SelectActivePackage(packages, late) == 0);

  // An unconditional package is vacuously true, so it always qualifies.
  base::Vector<PackageDef> fallback = {leg(40), PackageDef{}};
  Check("an unconditional package is the fallback", SelectActivePackage(fallback, before) == 1);
  Check("empty list selects nothing", SelectActivePackage({}, before) == -1);
}

void TestEmptyAndTruncated() {
  std::puts("package: empty/truncated does not crash:");
  bethesda::Record empty;
  PackageDef d0 = ParsePackageRecord(7, empty);
  Check("empty: no target",
        d0.handle == 7 && d0.target.kind == PackageTarget::Kind::kNone && !d0.is_travel);

  // A PLDT shorter than the 12-byte struct must read as zeros, not over-read.
  Buffers buf;
  bethesda::Record r;
  Add(r, kPkdt, buf.Pkdt(0, 18));
  Add(r, kAnam, buf.Str("Location"));
  u8 stub[3] = {0, 0, 0};
  Add(r, kPldt, buf.Bytes(stub, sizeof(stub)));
  PackageDef d1 = ParsePackageRecord(8, r);
  Check("truncated PLDT: ref 0", d1.target.ref == 0);
}

// Dumps real travel/escort packages when a data dir is given. Validation only;
// not part of the deterministic gate.
int DumpReal(const base::String& data_dir) {
  using namespace rx::bethesda;
  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) {
    std::printf("failed to load records\n");
    return 1;
  }

  int travel = 0;
  records.EachOfType(
      FourCc('P', 'A', 'C', 'K'), [&](GlobalFormId id, const RecordStore::StoredRecord&) {
        Record rec;
        if (!records.Parse(id, &rec))
          return;
        base::String edid = rec.GetString(FourCc('E', 'D', 'I', 'D'));
        bool interesting = edid.find("MQ101") != base::String::npos ||
                           edid.find("Travel") != base::String::npos ||
                           edid.find("Escort") != base::String::npos;
        if (!interesting)
          return;
        PackageDef def = ParsePackageRecord(id.packed(), rec, records);
        if (def.target.kind == PackageTarget::Kind::kNone)
          return;
        ++travel;
        const char* k = def.target.kind == PackageTarget::Kind::kReference   ? "reference"
                        : def.target.kind == PackageTarget::Kind::kAlias     ? "alias"
                        : def.target.kind == PackageTarget::Kind::kLinkedRef ? "linkedref"
                        : def.target.kind == PackageTarget::Kind::kSelf      ? "self"
                                                                             : "location";
        std::printf("PACK %04x:%06x %-44s travel=%d %-9s ref=%llx alias=%d r=%.0f\n", id.plugin,
                    id.local_id, edid.c_str(), def.is_travel ? 1 : 0, k,
                    (unsigned long long)def.target.ref, def.target.alias, def.target.radius);
      });
  std::printf("travel-ish packages parsed: %d\n", travel);
  return travel > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  TestRefTravel();
  TestAliasTravel();
  TestRadiusTravel();
  TestHoldPosition();
  TestLocationAlias();
  TestEmptyAndTruncated();
  TestSelectActivePackage();

  int rc = 0;
  if (argc >= 2)
    rc = DumpReal(argv[1]);

  if (g_failures == 0 && rc == 0) {
    std::puts("package: all checks passed");
    return 0;
  }
  std::printf("package: %d unit checks FAILED (dump rc=%d)\n", g_failures, rc);
  return 1;
}
