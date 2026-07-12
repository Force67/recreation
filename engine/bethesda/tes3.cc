// TES3 (Morrowind) -> modern record model translation. See tes3.h for the
// shape of the mapping. Layouts verified against the real Morrowind.esm:
// record header name[4]+size+unknown+flags (size excludes the header),
// subrecords name[4]+u32 size, VHGT 4+65*65+3 row-accumulated i8 deltas in
// units of 8, VTEX 16x16 u16 LTEX indices stored in 4x4 chunks, exterior
// cell references inline as FRMR/NAME/XSCL/DATA runs.

#include "bethesda/tes3.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"

namespace rx::bethesda {
namespace {

constexpr u32 kTes3 = FourCc('T', 'E', 'S', '3');
constexpr u32 kHedr = FourCc('H', 'E', 'D', 'R');
constexpr u32 kName = FourCc('N', 'A', 'M', 'E');
constexpr u32 kModl = FourCc('M', 'O', 'D', 'L');
constexpr u32 kData = FourCc('D', 'A', 'T', 'A');
constexpr u32 kDele = FourCc('D', 'E', 'L', 'E');
constexpr u32 kIntv = FourCc('I', 'N', 'T', 'V');
constexpr u32 kFrmr = FourCc('F', 'R', 'M', 'R');
constexpr u32 kXscl = FourCc('X', 'S', 'C', 'L');
constexpr u32 kVhgt = FourCc('V', 'H', 'G', 'T');
constexpr u32 kVnml = FourCc('V', 'N', 'M', 'L');
constexpr u32 kVclr = FourCc('V', 'C', 'L', 'R');
constexpr u32 kVtex = FourCc('V', 'T', 'E', 'X');
constexpr u32 kLhdt = FourCc('L', 'H', 'D', 'T');

constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kLand = FourCc('L', 'A', 'N', 'D');
constexpr u32 kLtex = FourCc('L', 'T', 'E', 'X');
constexpr u32 kLigh = FourCc('L', 'I', 'G', 'H');
constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
constexpr u32 kRefr = FourCc('R', 'E', 'F', 'R');
constexpr u32 kTxst = FourCc('T', 'X', 'S', 'T');
constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kXclc = FourCc('X', 'C', 'L', 'C');
constexpr u32 kTnam = FourCc('T', 'N', 'A', 'M');
constexpr u32 kTx00 = FourCc('T', 'X', '0', '0');
constexpr u32 kBtxt = FourCc('B', 'T', 'X', 'T');
constexpr u32 kAtxt = FourCc('A', 'T', 'X', 'T');
constexpr u32 kVtxt = FourCc('V', 'T', 'X', 'T');

constexpr u32 kTes3GridPoints = 65;   // 65x65 LAND vertices per 8192-unit cell
constexpr u32 kGridPoints = 33;       // 33x33 per virtual 4096-unit cell
constexpr u32 kQuadGrid = 17;         // VTXT opacity grid per quadrant
constexpr u16 kCellFlagHasWater = 0x2;
constexpr f32 kVirtualCellSize = 4096.0f;

struct SubSpan {
  u32 type;
  ByteSpan data;
};

struct Tes3Record {
  u32 type;
  u32 flags;
  ByteSpan payload;
};

// Base record types worth carrying over: everything whose MODL names a world
// model the streamer can place (matching BaseTypeHasWorldModel), plus a few
// TES3-only clutter types that simply get skipped downstream.
bool IsBaseType(u32 type) {
  switch (type) {
    case FourCc('S', 'T', 'A', 'T'):
    case FourCc('A', 'C', 'T', 'I'):
    case FourCc('D', 'O', 'O', 'R'):
    case FourCc('C', 'O', 'N', 'T'):
    case FourCc('M', 'I', 'S', 'C'):
    case FourCc('A', 'L', 'C', 'H'):
    case FourCc('I', 'N', 'G', 'R'):
    case FourCc('B', 'O', 'O', 'K'):
    case FourCc('W', 'E', 'A', 'P'):
    case FourCc('A', 'R', 'M', 'O'):
    case FourCc('C', 'L', 'O', 'T'):
    case FourCc('A', 'P', 'P', 'A'):
    case FourCc('L', 'O', 'C', 'K'):
    case FourCc('P', 'R', 'O', 'B'):
    case FourCc('R', 'E', 'P', 'A'):
    case FourCc('L', 'I', 'G', 'H'):
      return true;
    default:
      return false;
  }
}

bool WalkSubrecords(ByteSpan payload, base::Vector<SubSpan>* out) {
  size_t pos = 0;
  while (pos + 8 <= payload.size()) {
    u32 type, size;
    std::memcpy(&type, payload.data() + pos, 4);
    std::memcpy(&size, payload.data() + pos + 4, 4);
    pos += 8;
    if (pos + size > payload.size()) return false;
    out->push_back({type, payload.subspan(pos, size)});
    pos += size;
  }
  return pos == payload.size();
}

const SubSpan* FindSub(const base::Vector<SubSpan>& subs, u32 type) {
  for (const SubSpan& sub : subs) {
    if (sub.type == type) return &sub;
  }
  return nullptr;
}

std::string SubString(const SubSpan& sub) {
  size_t len = sub.data.size();
  while (len > 0 && sub.data[len - 1] == 0) --len;
  return std::string(reinterpret_cast<const char*>(sub.data.data()), len);
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

u32 GridKey(i32 x, i32 y) {
  return static_cast<u32>(static_cast<u16>(static_cast<i16>(x))) << 16 |
         static_cast<u16>(static_cast<i16>(y));
}

// Builds one synthesized record: subrecords appended in modern (u16 size)
// encoding, then sealed with the record header + group context.
class Emitter {
 public:
  explicit Emitter(Tes3Translation* out) : out_(out) {}

  void Begin(u32 type, u32 form_id, const GroupContext& ctx) {
    rec_ = Tes3Translation::Rec{};
    rec_.header.type = type;
    rec_.header.form_id = RawFormId{form_id};
    rec_.ctx = ctx;
    rec_.payload_offset = static_cast<u32>(out_->arena.size());
  }

  void Sub(u32 type, const void* data, size_t size) {
    // No TES3 subrecord exceeds 64K (verified over the full esm), so the
    // modern u16 size always fits.
    u16 size16 = static_cast<u16>(size);
    Append(&type, 4);
    Append(&size16, 2);
    Append(data, size);
  }

  void SubString(u32 type, const std::string& s) { Sub(type, s.c_str(), s.size() + 1); }

  void End() {
    rec_.payload_size = static_cast<u32>(out_->arena.size()) - rec_.payload_offset;
    rec_.header.data_size = rec_.payload_size;
    out_->records.push_back(rec_);
  }

 private:
  void Append(const void* data, size_t size) {
    size_t at = out_->arena.size();
    // base::Vector::resize grows capacity exactly; double it by hand or the
    // arena append turns quadratic over the ~100MB result.
    if (at + size > out_->arena.capacity()) {
      out_->arena.reserve(std::max<size_t>((at + size) * 2, 1u << 20));
    }
    out_->arena.resize(at + size);
    std::memcpy(out_->arena.data() + at, data, size);
  }

  Tes3Translation* out_;
  Tes3Translation::Rec rec_;
};

// Decodes a TES3 VHGT (65x65) into absolute heights in units of 8.
bool DecodeTes3Heights(ByteSpan vhgt, f32* out) {
  constexpr u32 n = kTes3GridPoints;
  if (vhgt.size() < 4 + n * n) return false;
  f32 offset;
  std::memcpy(&offset, vhgt.data(), 4);
  const i8* deltas = reinterpret_cast<const i8*>(vhgt.data() + 4);
  f32 row_start = offset;
  for (u32 r = 0; r < n; ++r) {
    row_start += static_cast<f32>(deltas[r * n]);
    f32 value = row_start;
    out[r * n] = value;
    for (u32 c = 1; c < n; ++c) {
      value += static_cast<f32>(deltas[r * n + c]);
      out[r * n + c] = value;
    }
  }
  return true;
}

i8 ClampDelta(f32 d) {
  return static_cast<i8>(std::clamp(d, -128.0f, 127.0f));
}

}  // namespace

bool TranslateTes3(ByteSpan data, Tes3Translation* out) {
  // ---- flat record walk ----
  base::Vector<Tes3Record> records;
  {
    size_t pos = 0;
    while (pos + 16 <= data.size()) {
      u32 type, size, flags;
      std::memcpy(&type, data.data() + pos, 4);
      std::memcpy(&size, data.data() + pos + 4, 4);
      std::memcpy(&flags, data.data() + pos + 12, 4);
      pos += 16;
      if (pos + size > data.size()) return false;
      records.push_back({type, flags, data.subspan(pos, size)});
      pos += size;
    }
    if (pos != data.size() || records.empty() || records[0].type != kTes3) return false;
  }

  {
    base::Vector<SubSpan> subs;
    if (!WalkSubrecords(records[0].payload, &subs)) return false;
    if (const SubSpan* hedr = FindSub(subs, kHedr); hedr && hedr->data.size() >= 300) {
      std::memcpy(&out->version, hedr->data.data(), 4);
      std::memcpy(&out->record_count, hedr->data.data() + 296, 4);
    }
  }

  // ---- pass 1: assign synthetic form ids ----
  u32 next_id = 1;
  auto take_id = [&next_id] { return next_id++; };

  std::unordered_map<std::string, u32> base_ids;   // lowercased string id -> form id
  std::unordered_map<u32, u32> ltex_by_index;      // LTEX INTV -> LTEX form id
  std::unordered_map<u32, u32> txst_by_ltex;       // LTEX form id -> TXST form id
  struct LtexInfo {
    u32 ltex_id;
    u32 txst_id;
    std::string name;
    std::string texture;
  };
  base::Vector<LtexInfo> ltexes;
  base::Vector<const Tes3Record*> bases;
  base::Vector<const Tes3Record*> cells;
  base::Vector<const Tes3Record*> lands;
  std::unordered_map<u32, u32> virtual_cells;  // virtual grid key -> CELL form id

  const u32 wrld_id = take_id();

  // The default land texture (VTEX index 0).
  {
    LtexInfo def;
    def.ltex_id = take_id();
    def.txst_id = take_id();
    def.name = "_tes3_land_default";
    def.texture = "_land_default.dds";
    ltex_by_index[0xffffffffu] = def.ltex_id;  // sentinel key for "index 0"
    txst_by_ltex[def.ltex_id] = def.txst_id;
    ltexes.push_back(std::move(def));
  }

  base::Vector<SubSpan> subs;
  for (size_t i = 1; i < records.size(); ++i) {
    const Tes3Record& rec = records[i];
    if (rec.flags & 0x20) continue;  // deleted
    if (rec.type == kLtex) {
      subs.clear();
      if (!WalkSubrecords(rec.payload, &subs)) continue;
      const SubSpan* name = FindSub(subs, kName);
      const SubSpan* intv = FindSub(subs, kIntv);
      const SubSpan* path = FindSub(subs, kData);
      if (!name || !intv || intv->data.size() < 4 || !path) continue;
      LtexInfo info;
      info.ltex_id = take_id();
      info.txst_id = take_id();
      info.name = SubString(*name);
      // The referenced .tga/.bmp ships as .dds inside the archives.
      info.texture = SubString(*path);
      if (size_t dot = info.texture.rfind('.'); dot != std::string::npos) {
        info.texture = info.texture.substr(0, dot);
      }
      info.texture += ".dds";
      u32 index;
      std::memcpy(&index, intv->data.data(), 4);
      ltex_by_index[index] = info.ltex_id;
      txst_by_ltex[info.ltex_id] = info.txst_id;
      ltexes.push_back(std::move(info));
    } else if (IsBaseType(rec.type)) {
      subs.clear();
      if (!WalkSubrecords(rec.payload, &subs)) continue;
      const SubSpan* name = FindSub(subs, kName);
      if (!name || FindSub(subs, kDele)) continue;
      base_ids.emplace(Lower(SubString(*name)), take_id());
      bases.push_back(&rec);
    } else if (rec.type == kCell) {
      subs.clear();
      if (!WalkSubrecords(rec.payload, &subs)) continue;
      const SubSpan* cdata = FindSub(subs, kData);
      if (!cdata || cdata->data.size() < 12) continue;
      u32 flags;
      i32 gx, gy;
      std::memcpy(&flags, cdata->data.data(), 4);
      std::memcpy(&gx, cdata->data.data() + 4, 4);
      std::memcpy(&gy, cdata->data.data() + 8, 4);
      if (flags & 0x1) continue;  // interior: not translated yet
      cells.push_back(&rec);
      for (i32 vy = gy * 2; vy < gy * 2 + 2; ++vy) {
        for (i32 vx = gx * 2; vx < gx * 2 + 2; ++vx) {
          virtual_cells.emplace(GridKey(vx, vy), take_id());
        }
      }
    } else if (rec.type == kLand) {
      lands.push_back(&rec);
    }
  }

  // ---- pass 2: emit in dependency order ----
  Emitter emit(out);
  GroupContext top;  // worldspace/cell zero: plain top level records

  emit.Begin(kWrld, wrld_id, top);
  emit.SubString(kEdid, "Vvardenfell");
  emit.End();

  for (const LtexInfo& info : ltexes) {
    emit.Begin(kTxst, info.txst_id, top);
    emit.SubString(kTx00, info.texture);
    emit.End();
    emit.Begin(kLtex, info.ltex_id, top);
    emit.SubString(kEdid, info.name);
    emit.Sub(kTnam, &info.txst_id, 4);
    emit.End();
  }

  for (const Tes3Record* rec : bases) {
    subs.clear();
    if (!WalkSubrecords(rec->payload, &subs)) continue;
    const SubSpan* name = FindSub(subs, kName);
    std::string id_string = SubString(*name);
    auto it = base_ids.find(Lower(id_string));
    if (it == base_ids.end()) continue;
    emit.Begin(rec->type, it->second, top);
    emit.SubString(kEdid, id_string);
    if (const SubSpan* modl = FindSub(subs, kModl)) {
      emit.Sub(kModl, modl->data.data(), modl->data.size());
    }
    if (rec->type == kLigh) {
      // LHDT: weight f32, value, time, radius, colour, flags. The modern LIGH
      // DATA the streamer reads is time @0, radius @4, colour @8.
      if (const SubSpan* lhdt = FindSub(subs, kLhdt); lhdt && lhdt->data.size() >= 20) {
        u8 ldata[12];
        std::memcpy(ldata, lhdt->data.data() + 8, 12);
        emit.Sub(kData, ldata, 12);
      }
    }
    emit.End();
  }

  // Virtual cells: 2x2 per exterior TES3 cell, all flooded at the (implicit)
  // sea level; the streamer's fallback water height supplies the 0.
  GroupContext world_ctx;
  world_ctx.worldspace = RawFormId{wrld_id};
  for (const Tes3Record* rec : cells) {
    subs.clear();
    if (!WalkSubrecords(rec->payload, &subs)) continue;
    const SubSpan* cdata = FindSub(subs, kData);
    i32 gx, gy;
    std::memcpy(&gx, cdata->data.data() + 4, 4);
    std::memcpy(&gy, cdata->data.data() + 8, 4);
    for (i32 vy = gy * 2; vy < gy * 2 + 2; ++vy) {
      for (i32 vx = gx * 2; vx < gx * 2 + 2; ++vx) {
        auto it = virtual_cells.find(GridKey(vx, vy));
        if (it == virtual_cells.end()) continue;
        emit.Begin(kCell, it->second, world_ctx);
        u16 flags16 = kCellFlagHasWater;
        emit.Sub(kData, &flags16, 2);
        i32 grid[2] = {vx, vy};
        emit.Sub(kXclc, grid, 8);
        emit.End();
      }
    }
  }

  // References: inline FRMR runs, emitted as REFR into the virtual cell their
  // position lands in (which handles the 2x2 split and cross-cell strays alike).
  size_t refs_emitted = 0, refs_skipped = 0;
  for (const Tes3Record* rec : cells) {
    subs.clear();
    if (!WalkSubrecords(rec->payload, &subs)) continue;
    size_t first_ref = subs.size();
    for (size_t s = 0; s < subs.size(); ++s) {
      if (subs[s].type == kFrmr) {
        first_ref = s;
        break;
      }
    }
    for (size_t s = first_ref; s < subs.size();) {
      // One reference: FRMR then its subrecords until the next FRMR.
      size_t end = s + 1;
      while (end < subs.size() && subs[end].type != kFrmr) ++end;
      const SubSpan* rname = nullptr;
      const SubSpan* rdata = nullptr;
      const SubSpan* rscale = nullptr;
      bool deleted = false;
      for (size_t k = s + 1; k < end; ++k) {
        if (subs[k].type == kName && !rname) rname = &subs[k];
        if (subs[k].type == kData && subs[k].data.size() >= 24) rdata = &subs[k];
        if (subs[k].type == kXscl && !rscale) rscale = &subs[k];
        if (subs[k].type == kDele) deleted = true;
      }
      s = end;
      if (deleted || !rname || !rdata) {
        ++refs_skipped;
        continue;
      }
      auto base = base_ids.find(Lower(SubString(*rname)));
      if (base == base_ids.end()) {
        ++refs_skipped;  // NPCs, creatures, leveled lists: not translated yet
        continue;
      }
      f32 pos[2];
      std::memcpy(pos, rdata->data.data(), 8);
      i32 vx = static_cast<i32>(std::floor(pos[0] / kVirtualCellSize));
      i32 vy = static_cast<i32>(std::floor(pos[1] / kVirtualCellSize));
      auto cell_it = virtual_cells.find(GridKey(vx, vy));
      if (cell_it == virtual_cells.end()) {
        ++refs_skipped;
        continue;
      }
      GroupContext ref_ctx = world_ctx;
      ref_ctx.cell = RawFormId{cell_it->second};
      ref_ctx.cell_group_type = 9;
      emit.Begin(kRefr, take_id(), ref_ctx);
      emit.Sub(kName, &base->second, 4);
      if (rscale && rscale->data.size() >= 4) emit.Sub(kXscl, rscale->data.data(), 4);
      emit.Sub(kData, rdata->data.data(), 24);
      emit.End();
      ++refs_emitted;
    }
  }

  // Terrain: each 65x65 TES3 LAND splits into four 33x33 quadrant LANDs.
  base::Vector<f32> heights(kTes3GridPoints * kTes3GridPoints);
  size_t lands_emitted = 0;
  for (const Tes3Record* rec : lands) {
    subs.clear();
    if (!WalkSubrecords(rec->payload, &subs)) continue;
    const SubSpan* intv = FindSub(subs, kIntv);
    const SubSpan* vhgt = FindSub(subs, kVhgt);
    if (!intv || intv->data.size() < 8 || !vhgt) continue;
    i32 gx, gy;
    std::memcpy(&gx, intv->data.data(), 4);
    std::memcpy(&gy, intv->data.data() + 4, 4);
    if (!DecodeTes3Heights(vhgt->data, heights.data())) continue;
    const SubSpan* vnml = FindSub(subs, kVnml);
    const SubSpan* vclr = FindSub(subs, kVclr);
    const SubSpan* vtex = FindSub(subs, kVtex);
    bool has_normals = vnml && vnml->data.size() >= kTes3GridPoints * kTes3GridPoints * 3;
    bool has_colors = vclr && vclr->data.size() >= kTes3GridPoints * kTes3GridPoints * 3;

    // Deswizzle the 16x16 texture grid (stored as 4x4 chunks of 4x4).
    u16 texture_grid[16 * 16] = {};
    bool has_textures = vtex && vtex->data.size() >= 512;
    if (has_textures) {
      const u8* raw = vtex->data.data();
      for (u32 chunk = 0; chunk < 16; ++chunk) {
        for (u32 t = 0; t < 16; ++t) {
          u16 v;
          std::memcpy(&v, raw + (chunk * 16 + t) * 2, 2);
          u32 x = (chunk % 4) * 4 + t % 4;
          u32 y = (chunk / 4) * 4 + t / 4;
          texture_grid[y * 16 + x] = v;
        }
      }
    }
    auto ltex_for = [&](u16 v) -> u32 {
      auto it = ltex_by_index.find(v == 0 ? 0xffffffffu : v - 1);
      if (it == ltex_by_index.end()) it = ltex_by_index.find(0xffffffffu);
      return it->second;
    };

    for (u32 quad_y = 0; quad_y < 2; ++quad_y) {
      for (u32 quad_x = 0; quad_x < 2; ++quad_x) {
        i32 vx = gx * 2 + static_cast<i32>(quad_x);
        i32 vy = gy * 2 + static_cast<i32>(quad_y);
        auto cell_it = virtual_cells.find(GridKey(vx, vy));
        if (cell_it == virtual_cells.end()) continue;
        const u32 r0 = quad_y * (kGridPoints - 1);
        const u32 c0 = quad_x * (kGridPoints - 1);

        GroupContext land_ctx = world_ctx;
        land_ctx.cell = RawFormId{cell_it->second};
        land_ctx.cell_group_type = 9;
        emit.Begin(kLand, take_id(), land_ctx);

        // VHGT re-encode: same row-accumulated scheme at 33x33. Row deltas are
        // the source deltas; only right-half first-column steps are re-derived
        // (clamped, they are vertical neighbor differences).
        u8 vhgt_out[4 + kGridPoints * kGridPoints + 3] = {};
        f32 offset = heights[r0 * kTes3GridPoints + c0];
        std::memcpy(vhgt_out, &offset, 4);
        i8* d = reinterpret_cast<i8*>(vhgt_out + 4);
        for (u32 r = 0; r < kGridPoints; ++r) {
          const f32* row = &heights[(r0 + r) * kTes3GridPoints + c0];
          if (r == 0) {
            d[0] = 0;
          } else {
            const f32* prev = &heights[(r0 + r - 1) * kTes3GridPoints + c0];
            d[r * kGridPoints] = ClampDelta(row[0] - prev[0]);
          }
          for (u32 c = 1; c < kGridPoints; ++c) {
            d[r * kGridPoints + c] = ClampDelta(row[c] - row[c - 1]);
          }
        }
        emit.Sub(kVhgt, vhgt_out, sizeof(vhgt_out));

        if (has_normals) {
          u8 vnml_out[kGridPoints * kGridPoints * 3];
          for (u32 r = 0; r < kGridPoints; ++r) {
            std::memcpy(vnml_out + r * kGridPoints * 3,
                        vnml->data.data() + ((r0 + r) * kTes3GridPoints + c0) * 3,
                        kGridPoints * 3);
          }
          emit.Sub(kVnml, vnml_out, sizeof(vnml_out));
        }
        if (has_colors) {
          u8 vclr_out[kGridPoints * kGridPoints * 3];
          for (u32 r = 0; r < kGridPoints; ++r) {
            std::memcpy(vclr_out + r * kGridPoints * 3,
                        vclr->data.data() + ((r0 + r) * kTes3GridPoints + c0) * 3,
                        kGridPoints * 3);
          }
          emit.Sub(kVclr, vclr_out, sizeof(vclr_out));
        }

        {
          // This virtual cell's 8x8 texels; each modern quadrant covers 4x4.
          // Without a VTEX the all-zero grid still emits the default base
          // layer (Morrowind's _land_default), not the engine fallback.
          for (u32 mq = 0; mq < 4; ++mq) {
            const u32 tx0 = quad_x * 8 + (mq & 1) * 4;
            const u32 ty0 = quad_y * 8 + (mq >> 1) * 4;
            u16 texels[16];
            for (u32 t = 0; t < 16; ++t) {
              texels[t] = texture_grid[(ty0 + t / 4) * 16 + tx0 + t % 4];
            }
            // Base layer: the most common texture in the quadrant.
            u16 best = texels[0];
            u32 best_count = 0;
            for (u32 a = 0; a < 16; ++a) {
              u32 count = 0;
              for (u32 b = 0; b < 16; ++b) count += texels[b] == texels[a];
              if (count > best_count) {
                best_count = count;
                best = texels[a];
              }
            }
            struct BtxtData {
              u32 ltex;
              u8 quadrant;
              u8 pad[3];
            } btxt{ltex_for(best), static_cast<u8>(mq), {}};
            emit.Sub(kBtxt, &btxt, 8);

            // One additive layer per remaining distinct texture, opacity 1
            // over the texels it covers (17x17 grid, ~4 points per texel).
            u16 done[16];
            u32 done_count = 0;
            done[done_count++] = best;
            u16 layer_index = 0;
            for (u32 a = 0; a < 16; ++a) {
              u16 v = texels[a];
              bool seen = false;
              for (u32 k = 0; k < done_count; ++k) seen |= done[k] == v;
              if (seen) continue;
              done[done_count++] = v;
              struct AtxtData {
                u32 ltex;
                u8 quadrant;
                u8 pad;
                u16 layer;
              } atxt{ltex_for(v), static_cast<u8>(mq), 0, layer_index++};
              emit.Sub(kAtxt, &atxt, 8);
              u8 vtxt[kQuadGrid * kQuadGrid * 8];
              u32 entries = 0;
              for (u32 g = 0; g < kQuadGrid * kQuadGrid; ++g) {
                u32 cx = std::min(3u, (g % kQuadGrid) / 4);
                u32 cy = std::min(3u, (g / kQuadGrid) / 4);
                if (texels[cy * 4 + cx] != v) continue;
                u16 position = static_cast<u16>(g);
                f32 opacity = 1.0f;
                std::memcpy(vtxt + entries * 8, &position, 2);
                std::memset(vtxt + entries * 8 + 2, 0, 2);
                std::memcpy(vtxt + entries * 8 + 4, &opacity, 4);
                ++entries;
              }
              emit.Sub(kVtxt, vtxt, entries * 8);
            }
          }
        }
        emit.End();
        ++lands_emitted;
      }
    }
  }

  RX_INFO("tes3: {} source records -> {} synthesized ({} bases, {} exterior cells -> {} virtual, "
           "{} refs placed / {} skipped, {} land quadrants, {} land textures)",
           records.size(), out->records.size(), bases.size(), cells.size(), virtual_cells.size(),
           refs_emitted, refs_skipped, lands_emitted, ltexes.size());
  return true;
}

}  // namespace rx::bethesda
