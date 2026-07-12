#include "bethesda/material_db.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace rx::bethesda {
namespace {

std::string ToLowerSlashes(std::string_view raw) {
  std::string out(raw);
  for (char& c : out) {
    if (c == '\\') c = '/';
    else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// "textures/..." key the vfs uses, from a stored "Data\Textures\...DDS" path.
std::string NormalizeTexturePath(std::string_view raw) {
  std::string path = ToLowerSlashes(raw);
  size_t anchor = path.find("textures/");
  return anchor == std::string::npos ? "textures/" + path : path.substr(anchor);
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The material stem for a TextureSet object named "<stem>_TextureSet<n>", or
// empty when the name is not a texture set (a composite path, a layer, etc.).
std::string TextureSetStem(std::string_view name) {
  std::string lower = ToLowerSlashes(name);
  size_t marker = lower.rfind("_textureset");
  if (marker == std::string::npos || marker == 0) return {};
  for (size_t i = marker + 11; i < lower.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(lower[i]))) return {};
  }
  return lower.substr(0, marker);
}

// --- BSComponentDB2 wire format ---------------------------------------------
// materialsbeta.cdb is a BSComponentDB2 reflection stream: a "BETH" header, a
// STRT string table, a TYPE/CLAS schema (97 classes), then three index LIST
// chunks (ObjectInfo/ComponentInfo/EdgeInfo) followed by a stream of OBJT/DIFF
// component chunks. Textures are reached by an object graph, not by adjacency:
//   Material -> LayerID -> Layer -> MaterialID -> Material -> TextureSetID ->
//   TextureSet -> TextureFile paths (slot 0 color, 1 normal, 7 emissive).
// A material's persistent id is a BSResourceID (path CRC), so a .mat path
// resolves by hashing it the same way. Reverse-engineered from
// gibbed/Gibbed.Starfield and fo76utils/nifskope.

constexpr u32 kChunkStrt = 0x54525453;  // "STRT"
constexpr u32 kChunkType = 0x45505954;  // "TYPE"
constexpr u32 kChunkClas = 0x53414C43;  // "CLAS"
constexpr u32 kChunkList = 0x5453494C;  // "LIST"
constexpr u32 kChunkObjt = 0x544A424F;  // "OBJT"
constexpr u32 kChunkDiff = 0x46464944;  // "DIFF"

// TextureSet slot indices, from the BSMaterial::TextureFile component key.
// Confirmed Starfield BSMaterial layout: 0 color, 1 normal, 2 opacity,
// 3 roughness, 4 metallic, 5 ambient occlusion, 6 height, 7 emissive, 8 glow.
constexpr u32 kSlotColor = 0;
constexpr u32 kSlotNormal = 1;
constexpr u32 kSlotRoughness = 3;
constexpr u32 kSlotMetallic = 4;
constexpr u32 kSlotAo = 5;
constexpr u32 kSlotEmissive = 7;
constexpr u32 kMaxSlots = 21;

// "mat\0" packed as read by the BSResourceID extension hash.
constexpr u32 kExtMat = 0x0074616D;

struct Cursor {
  const u8* p;
  const u8* end;
  bool Has(size_t n) const { return static_cast<size_t>(end - p) >= n; }
  u8 U8() { u8 v = *p; p += 1; return v; }
  u16 U16() { u16 v; std::memcpy(&v, p, 2); p += 2; return v; }
  u32 U32() { u32 v; std::memcpy(&v, p, 4); p += 4; return v; }
  u64 U64() { u64 v; std::memcpy(&v, p, 8); p += 8; return v; }
};

const std::uint32_t& Crc32Table(size_t i) {
  static const auto table = [] {
    std::array<u32, 256> t{};
    for (u32 n = 0; n < 256; ++n) {
      u32 c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[n] = c;
    }
    return t;
  }();
  return table[i];
}

void Crc32Byte(u32& h, unsigned char c) { h = (h >> 8) ^ Crc32Table((h ^ c) & 0xFF); }

}  // namespace

// A BSResourceID is {dir CRC, base-name CRC, extension}, matching the Starfield
// resource path hash so a .mat path resolves against the compiled objects.
StarfieldMaterialDb::ResourceId StarfieldMaterialDb::HashResource(std::string_view path) {
  size_t base = path.rfind('/');
  size_t base2 = path.rfind('\\');
  if (base == std::string_view::npos || (base2 != std::string_view::npos && base2 > base)) base = base2;
  size_t ext = path.rfind('.');
  if (ext == std::string_view::npos || (base != std::string_view::npos && ext < base)) ext = path.size();

  size_t i = 0;
  u32 crc = 0;
  if (base != std::string_view::npos) {
    for (; i < base; ++i) {
      unsigned char c = static_cast<unsigned char>(path[i]);
      if (c >= 'A' && c <= 'Z') c |= 0x20;
      else if (c == '/') c = '\\';
      Crc32Byte(crc, c);
    }
    ++i;
  }
  ResourceId id;
  id.dir = crc;
  crc = 0;
  for (; i < ext; ++i) {
    unsigned char c = static_cast<unsigned char>(path[i]);
    if (c >= 'A' && c <= 'Z') c |= 0x20;
    Crc32Byte(crc, c);
  }
  id.file = crc;

  u32 e = 0;
  size_t remain = path.size() - i;
  const char* d = path.data();
  if (remain >= 5) std::memcpy(&e, d + i + 1, 4);
  else if (remain == 4) { u32 t; std::memcpy(&t, d + i, 4); e = t >> 8; }
  else if (remain == 3) { u16 t; std::memcpy(&t, d + i + 1, 2); e = t; }
  else if (remain == 2) e = static_cast<unsigned char>(d[i + 1]);
  id.ext = e | ((e >> 1) & 0x20202020u);
  return id;
}

void StarfieldMaterialDb::Build(ByteSpan cdb) {
  by_stem_.clear();
  by_resource_.clear();
  if (cdb.size() < 16) return;

  BuildGraphIndex(cdb);
  // Keep the TextureSet-name scan as a fallback: it recovers materials whose
  // texture set is named "<material>_TextureSet<n>" (mostly landscape) and any
  // graph edge this reader could not follow, so net coverage only grows.
  BuildStemIndex(cdb);
}

void StarfieldMaterialDb::BuildGraphIndex(ByteSpan cdb) {
  Cursor r{cdb.data(), cdb.data() + cdb.size()};
  r.p += 16;  // BETH magic + headerSize(8) + version(4) + chunkCount

  auto read_chunk = [&](u32& type, u32& size) -> bool {
    if (!r.Has(8)) return false;
    type = r.U32();
    size = r.U32();
    return r.Has(size);
  };

  u32 ct, cs;
  if (!read_chunk(ct, cs) || ct != kChunkStrt) return;
  const char* strt = reinterpret_cast<const char*>(r.p);
  u32 strt_size = cs;
  r.p += cs;
  auto str_at = [&](u32 off) -> std::string_view {
    if (off >= strt_size) return {};
    return std::string_view(strt + off);
  };
  // A field/class type offset is a STRT offset, or a builtin encoded as
  // >= 0xFFFFFF01; we only need to distinguish String and the ID-carrying
  // classes, so map builtins to their names.
  auto type_at = [&](u32 off) -> std::string_view {
    if (off < 0xFFFFFF01u) return str_at(off);
    switch (off) {
      case 0xFFFFFF02u: return "String";
      case 0xFFFFFF03u: return "List";
      case 0xFFFFFF04u: return "Map";
      case 0xFFFFFF05u: return "Ref";
      case 0xFFFFFF08u: case 0xFFFFFF09u: return "1";  // Int8/UInt8/Bool width
      case 0xFFFFFF10u: return "1";
      case 0xFFFFFF0Au: case 0xFFFFFF0Bu: return "2";  // Int16/UInt16
      case 0xFFFFFF0Cu: case 0xFFFFFF0Du: case 0xFFFFFF11u: return "4";  // Int32/UInt32/Float
      case 0xFFFFFF0Eu: case 0xFFFFFF0Fu: case 0xFFFFFF12u: return "8";  // Int64/UInt64/Double
      default: return {};
    }
  };

  struct Field {
    std::string_view name;
    std::string_view type;
  };
  struct ClassDef {
    bool is_user = false;
    std::vector<Field> fields;
  };
  std::unordered_map<std::string, ClassDef> classes;

  if (!read_chunk(ct, cs) || ct != kChunkType) return;
  {
    Cursor tr{r.p, r.p + cs};
    u32 class_count = tr.Has(4) ? tr.U32() : 0;
    r.p += cs;
    for (u32 i = 0; i < class_count; ++i) {
      u32 cct, ccs;
      if (!read_chunk(cct, ccs) || cct != kChunkClas) return;
      Cursor cr{r.p, r.p + ccs};
      r.p += ccs;
      if (!cr.Has(12)) continue;
      std::string name(str_at(cr.U32()));
      cr.U32();  // class version
      u16 flags = cr.U16();
      u16 field_count = cr.U16();
      ClassDef cd;
      cd.is_user = (flags & 4) != 0;
      for (u16 j = 0; j < field_count && cr.Has(12); ++j) {
        Field f;
        f.name = str_at(cr.U32());
        f.type = type_at(cr.U32());
        cr.U16();  // data offset
        cr.U16();  // data size
        cd.fields.push_back(f);
      }
      classes.emplace(std::move(name), std::move(cd));
    }
  }

  // Per-object accumulated links and texture paths, indexed by dbID.
  struct Object {
    ResourceId id{};
    u32 layer_ids[8] = {0};
    u32 material_id = 0;
    u32 texture_set_id = 0;
    std::string tex[kMaxSlots];
    bool has_layer = false;
  };
  std::vector<Object> objects;
  auto grow = [&](u32 db_id) { if (objects.size() <= db_id) objects.resize(db_id + 1); };

  struct ComponentRef {
    u32 db_id;
    u32 key;  // (type << 16) | slot index
  };
  std::vector<ComponentRef> component_info;
  size_t component_cursor = 0;

  // Reads a String value: u16 length, then bytes (may be null-terminated).
  auto read_string = [&](Cursor& c) -> std::string {
    if (!c.Has(2)) return {};
    u16 len = c.U16();
    if (!c.Has(len)) return {};
    const char* s = reinterpret_cast<const char*>(c.p);
    c.p += len;
    while (len > 0 && s[len - 1] == '\0') --len;
    return std::string(s, len);
  };

  // Walks a schema-typed value, capturing the last String or ID (u32) leaf. The
  // components we resolve (LayerID/MaterialID/TextureSetID = one BSComponentDB2::ID
  // u32; TextureFile = one String FileName) are flat, so a schema-driven field
  // walk suffices; any component with a nested List/Map/user field is skipped.
  u32 last_id = 0;
  std::string last_str;
  bool have_id = false, have_str = false;
  std::function<void(Cursor&, std::string_view, bool, int)> read_item =
      [&](Cursor& c, std::string_view type_name, bool is_diff, int depth) {
        if (depth > 12) { c.p = c.end; return; }
        if (type_name == "String") { last_str = read_string(c); have_str = true; return; }
        if (type_name == "1") { if (c.Has(1)) c.U8(); return; }
        if (type_name == "2") { if (c.Has(2)) c.U16(); return; }
        if (type_name == "4") { if (c.Has(4)) c.U32(); return; }
        if (type_name == "8") { if (c.Has(8)) c.U64(); return; }
        auto it = classes.find(std::string(type_name));
        if (it == classes.end()) { c.p = c.end; return; }
        const ClassDef& cd = it->second;
        // BSComponentDB2::ID: a single UInt32 object id.
        if (type_name == "BSComponentDB2::ID") {
          if (is_diff) {
            while (c.Has(2)) {
              u16 fn = c.U16();
              if (static_cast<std::int16_t>(fn) < 0 || fn >= cd.fields.size()) break;
              if (c.Has(4)) { last_id = c.U32(); have_id = true; }
            }
          } else if (c.Has(4)) {
            last_id = c.U32();
            have_id = true;
          }
          return;
        }
        auto walk_field = [&](const Field& f) -> bool {
          auto ci = classes.find(std::string(f.type));
          if (f.type == "List" || f.type == "Map" || (ci != classes.end() && ci->second.is_user)) {
            c.p = c.end;  // nested chunk lives elsewhere; bail
            return false;
          }
          read_item(c, f.type, is_diff, depth + 1);
          return true;
        };
        if (is_diff) {
          while (c.Has(2)) {
            u16 fn = c.U16();
            if (static_cast<std::int16_t>(fn) < 0 || fn >= cd.fields.size()) break;
            if (!walk_field(cd.fields[fn])) return;
          }
        } else {
          for (const Field& f : cd.fields) {
            if (!walk_field(f)) return;
          }
        }
      };

  u32 type, size;
  while (read_chunk(type, size)) {
    const u8* data = r.p;
    r.p += size;

    if (type == kChunkObjt || type == kChunkDiff) {
      Cursor c{data, data + size};
      std::string_view class_name = c.Has(4) ? str_at(c.U32()) : std::string_view{};
      if (component_cursor >= component_info.size()) continue;
      const ComponentRef& ref = component_info[component_cursor++];
      u32 slot = ref.key & 0xFFFF;
      have_id = have_str = false;
      last_id = 0;
      last_str.clear();
      read_item(c, class_name, type == kChunkDiff, 0);
      if (ref.db_id >= objects.size()) continue;
      Object& o = objects[ref.db_id];
      if (class_name == "BSMaterial::LayerID" && have_id) {
        if (slot < 8) { o.layer_ids[slot] = last_id; o.has_layer = true; }
      } else if (class_name == "BSMaterial::MaterialID" && have_id) {
        o.material_id = last_id;
      } else if (class_name == "BSMaterial::TextureSetID" && have_id) {
        o.texture_set_id = last_id;
      } else if ((class_name == "BSMaterial::TextureFile" ||
                  class_name == "BSMaterial::MRTextureFile") &&
                 have_str) {
        if (slot < kMaxSlots && o.tex[slot].empty()) o.tex[slot] = std::move(last_str);
      }
      continue;
    }

    if (type != kChunkList) continue;
    Cursor c{data, data + size};
    std::string_view class_name = c.Has(4) ? str_at(c.U32()) : std::string_view{};
    u32 n = c.Has(4) ? c.U32() : 0;
    if (class_name == "BSComponentDB2::DBFileIndex::ObjectInfo") {
      // Entry: persistentID{file,ext,dir}(12), dbID(4), baseObjectID(4), then a
      // trailing hasData byte (21 total), or an extra parentID (33) in newer files.
      size_t remain = static_cast<size_t>(c.end - c.p);
      u32 entry = (n && remain / n >= 33) ? 33 : 21;
      for (u32 i = 0; i < n && c.Has(entry); ++i) {
        const u8* e = c.p;
        c.p += entry;
        u32 file, ext, dir, db_id;
        std::memcpy(&file, e + 0, 4);
        std::memcpy(&ext, e + 4, 4);
        std::memcpy(&dir, e + 8, 4);
        std::memcpy(&db_id, e + 12, 4);
        if (!db_id) continue;
        grow(db_id);
        objects[db_id].id = {file, ext, dir};
      }
    } else if (class_name == "BSComponentDB2::DBFileIndex::ComponentInfo") {
      for (u32 i = 0; i < n && c.Has(8); ++i) {
        u32 db_id = c.U32();
        u32 key = c.U32();
        component_info.push_back({db_id, key});
      }
      component_cursor = 0;
    }
    // EdgeInfo carries the parent/child hierarchy; the texture graph is reached
    // through the ID components above, so the edge table is not needed here.
  }

  // A layer's color texture is a generic overlay (grunge/dust/detail/tint/blank
  // pattern) rather than the surface's actual material, when it lives under
  // common\grunge / common\detail or its file name marks it as a grunge/detail/
  // blend/tint/blank overlay. Starfield stacks these as layer 0 to weather a
  // dominant lower layer through a blend mask; binding the overlay as the whole
  // base colour makes a granite rock read as flat dust. Preferring the first
  // non-overlay layer's textures picks the real surface (the closest single-map
  // approximation of the multi-layer blend). Slot-independent: normal/PBR follow
  // the chosen layer set.
  auto is_generic_overlay = [](std::string_view path) {
    std::string lower(path);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lower) if (c == '\\') c = '/';
    if (lower.find("/grunge/") != std::string::npos ||
        lower.find("/detail/") != std::string::npos)
      return true;
    size_t slash = lower.rfind('/');
    std::string_view name = slash == std::string::npos ? lower : std::string_view(lower).substr(slash + 1);
    return name.find("grunge") != std::string_view::npos ||
           name.find("detail") != std::string_view::npos ||
           name.find("blank") != std::string_view::npos ||
           name.find("_tint") != std::string_view::npos ||
           name.find("blend") != std::string_view::npos;
  };

  // RX_STARFIELD_LAYER_PICK=0 pins the original behaviour (first layer with a
  // texture in each slot, independently) for A/B; default prefers a real surface
  // layer over a generic overlay for the whole texture set.
  const char* pick_env = std::getenv("RX_STARFIELD_LAYER_PICK");
  const bool prefer_surface = !pick_env || pick_env[0] != '0';

  // The chosen texture set for a material: the first layer whose colour is a
  // real surface (not a generic overlay), else the first layer with any colour,
  // else the first layer. Reading every slot from this one set keeps a material
  // internally consistent (colour/normal/PBR from the same surface).
  auto pick_layer = [&](const Object& mat) -> u32 {
    u32 first_with_color = 0, first_any = 0;
    for (u32 layer : mat.layer_ids) {
      if (!layer || layer >= objects.size()) continue;
      u32 mid = objects[layer].material_id;
      if (!mid || mid >= objects.size()) continue;
      u32 tsid = objects[mid].texture_set_id;
      if (!tsid || tsid >= objects.size()) continue;
      if (!first_any) first_any = tsid;
      const std::string& c = objects[tsid].tex[kSlotColor];
      if (c.empty()) continue;
      if (!first_with_color) first_with_color = tsid;
      if (!is_generic_overlay(c)) return tsid;  // a real surface layer
    }
    return first_with_color ? first_with_color : first_any;
  };

  // Original path: first layer (in order) with a texture in this slot.
  auto tex_of_first = [&](const Object& mat, u32 slot) -> const std::string* {
    for (u32 layer : mat.layer_ids) {
      if (!layer || layer >= objects.size()) continue;
      u32 mid = objects[layer].material_id;
      if (!mid || mid >= objects.size()) continue;
      u32 tsid = objects[mid].texture_set_id;
      if (!tsid || tsid >= objects.size()) continue;
      const std::string& t = objects[tsid].tex[slot];
      if (!t.empty()) return &t;
    }
    return nullptr;
  };

  for (const Object& o : objects) {
    if (o.id.ext != kExtMat || !o.has_layer) continue;
    u32 tsid = prefer_surface ? pick_layer(o) : 0;
    auto tex_of = [&](u32 slot) -> const std::string* {
      if (!prefer_surface) return tex_of_first(o, slot);
      if (!tsid || tsid >= objects.size()) return nullptr;
      const std::string& t = objects[tsid].tex[slot];
      return t.empty() ? nullptr : &t;
    };
    const std::string* color = tex_of(kSlotColor);
    const std::string* normal = tex_of(kSlotNormal);
    const std::string* emissive = tex_of(kSlotEmissive);
    const std::string* roughness = tex_of(kSlotRoughness);
    const std::string* metallic = tex_of(kSlotMetallic);
    const std::string* ao = tex_of(kSlotAo);
    if (!color && !normal && !emissive && !roughness && !metallic && !ao) continue;
    Textures t;
    if (color) t.base_color = NormalizeTexturePath(*color);
    if (normal) t.normal = NormalizeTexturePath(*normal);
    if (emissive) t.emissive = NormalizeTexturePath(*emissive);
    if (roughness) t.roughness = NormalizeTexturePath(*roughness);
    if (metallic) t.metallic = NormalizeTexturePath(*metallic);
    if (ao) t.ao = NormalizeTexturePath(*ao);
    by_resource_.emplace(o.id, std::move(t));
  }
}

// Linear scan preserved from the original reader: a TextureSet object's name
// (component-type id 500) is immediately followed by its texture file paths
// (id 906) until the next name, so a stem index recovers most non-architecture
// materials without walking the graph.
void StarfieldMaterialDb::BuildStemIndex(ByteSpan cdb) {
  constexpr u32 kFieldTextureSetName = 500;
  constexpr u32 kFieldTextureFile = 906;

  std::string cur_stem;
  Textures cur;
  auto commit = [&]() {
    if (!cur_stem.empty() && !cur.base_color.empty())
      by_stem_.emplace(std::move(cur_stem), std::move(cur));
  };

  size_t p = 16;
  while (p + 8 <= cdb.size()) {
    const u8* chunk = cdb.data() + p;
    bool ascii = true;
    for (int i = 0; i < 4; ++i)
      if (chunk[i] < 32 || chunk[i] >= 127) ascii = false;
    if (!ascii) break;
    u32 size;
    std::memcpy(&size, chunk + 4, 4);
    if (p + 8 + static_cast<size_t>(size) > cdb.size()) break;
    const u8* data = chunk + 8;

    if (std::memcmp(chunk, "DIFF", 4) == 0 && size >= 8) {
      u32 fid;
      std::memcpy(&fid, data, 4);
      if (fid == kFieldTextureSetName || fid == kFieldTextureFile) {
        u16 len;
        std::memcpy(&len, data + 6, 2);
        if (8u + len <= size) {
          std::string_view value(reinterpret_cast<const char*>(data + 8), len);
          if (size_t z = value.find('\0'); z != std::string_view::npos) value = value.substr(0, z);
          if (fid == kFieldTextureSetName) {
            commit();
            cur = Textures{};
            cur_stem = TextureSetStem(value);
          } else if (!cur_stem.empty()) {
            std::string norm = NormalizeTexturePath(value);
            if (cur.base_color.empty() && EndsWith(norm, "_color.dds")) cur.base_color = std::move(norm);
            else if (cur.normal.empty() && EndsWith(norm, "_normal.dds")) cur.normal = std::move(norm);
            else if (cur.emissive.empty() && EndsWith(norm, "_emissive.dds")) cur.emissive = std::move(norm);
            else if (cur.roughness.empty() && EndsWith(norm, "_rough.dds")) cur.roughness = std::move(norm);
            else if (cur.metallic.empty() && EndsWith(norm, "_metal.dds")) cur.metallic = std::move(norm);
            else if (cur.ao.empty() && EndsWith(norm, "_ao.dds")) cur.ao = std::move(norm);
          }
        }
      }
    }
    p += 8 + size;
  }
  commit();
}

bool StarfieldMaterialDb::Lookup(std::string_view mat_path, Resolved* out) const {
  auto fill = [&](const Textures& t) {
    out->base_color = t.base_color;
    out->normal = t.normal;
    out->emissive = t.emissive;
    out->roughness = t.roughness;
    out->metallic = t.metallic;
    out->ao = t.ao;
    // A bound metallic map is the metal signal (no scalar metalness in the
    // TextureFile leaves this reader follows).
    out->metallic_hint = !t.metallic.empty();
  };

  // Prefer the object-graph result (resolves architecture/ships by path hash).
  if (auto it = by_resource_.find(HashResource(mat_path)); it != by_resource_.end()) {
    fill(it->second);
    return true;
  }

  std::string lower = ToLowerSlashes(mat_path);
  size_t slash = lower.find_last_of('/');
  std::string_view file = slash == std::string::npos ? std::string_view(lower)
                                                     : std::string_view(lower).substr(slash + 1);
  if (EndsWith(file, ".mat")) file = file.substr(0, file.size() - 4);

  auto it = by_stem_.find(std::string(file));
  if (it == by_stem_.end()) return false;
  fill(it->second);
  return true;
}

bool StarfieldMaterialDb::Lookup(std::string_view mat_path, std::string* base_color,
                                 std::string* normal, std::string* emissive) const {
  Resolved r;
  if (!Lookup(mat_path, &r)) return false;
  if (base_color && !r.base_color.empty()) *base_color = r.base_color;
  if (normal && !r.normal.empty()) *normal = r.normal;
  if (emissive && !r.emissive.empty()) *emissive = r.emissive;
  return true;
}

}  // namespace rx::bethesda
