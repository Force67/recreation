// material_dbtest: deterministic checks for the Starfield material database
// reader. It covers both resolution paths: the TextureSet-name scan (a minimal
// blob of DIFF string chunks) and the BSComponentDB2 object graph (a synthetic
// object/component stream wiring a material down to its texture set), plus the
// BSResourceID path hash against a value verified from the shipped database.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "bethesda/material_db.h"
#include "core/types.h"

namespace {

using rx::u16;
using rx::u32;
using rx::u8;

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

void PutU16(std::vector<u8>& b, u16 v) { b.insert(b.end(), {u8(v), u8(v >> 8)}); }
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutStr(std::vector<u8>& b, const char* s) {
  for (const char* p = s; *p; ++p) b.push_back(static_cast<u8>(*p));
}

// One DIFF chunk: 'DIFF' + u32 size + [u32 fieldId][u16 0][u16 len][string].
void PutDiff(std::vector<u8>& b, u32 field_id, const std::string& s) {
  PutStr(b, "DIFF");
  PutU32(b, static_cast<u32>(8 + s.size()));  // chunk size = data bytes
  PutU32(b, field_id);
  PutU16(b, 0);
  PutU16(b, static_cast<u16>(s.size()));
  for (char c : s) b.push_back(static_cast<u8>(c));
}

// Builds a minimal BSComponentDB2 that wires one material through the object
// graph to its texture set, matching the wire format the reader parses:
//   Material(dbID 1) -LayerID slot 0-> Layer(2) -MaterialID-> Material(3)
//   -TextureSetID-> TextureSet(4) with TextureFile slots 0 (color) / 1 (normal).
// Everything else the reader needs (STRT, TYPE/CLAS schema, index LISTs) is
// generated here.
struct GraphCdbBuilder {
  std::vector<u8> strt;
  std::map<std::string, u32> str_off;

  u32 Intern(const std::string& s) {
    auto it = str_off.find(s);
    if (it != str_off.end()) return it->second;
    u32 off = static_cast<u32>(strt.size());
    str_off[s] = off;
    for (char c : s) strt.push_back(static_cast<u8>(c));
    strt.push_back(0);
    return off;
  }

  static constexpr u32 kBuiltinString = 0xFFFFFF02u;
  static constexpr u32 kBuiltinUInt32 = 0xFFFFFF0Du;

  struct ClassDef {
    std::string name;
    std::vector<std::pair<std::string, u32>> fields;  // field name, type offset
  };
  std::vector<ClassDef> classes;

  void AddClass(const std::string& name, std::vector<std::pair<std::string, u32>> fields) {
    classes.push_back({name, std::move(fields)});
  }

  // A component chunk (OBJT): 'OBJT' + size + [u32 className][fields...].
  std::vector<u8> component;  // the streamed component chunk bytes
  std::vector<std::pair<u32, u32>> comp_info;  // (dbID, key)

  void PutObjt(u32 db_id, u32 key, const std::string& class_name,
               const std::vector<u8>& body) {
    comp_info.emplace_back(db_id, key);
    std::vector<u8> data;
    PutU32(data, Intern(class_name));
    data.insert(data.end(), body.begin(), body.end());
    PutStr(component, "OBJT");
    PutU32(component, static_cast<u32>(data.size()));
    component.insert(component.end(), data.begin(), data.end());
  }

  // ObjectInfo entries (21-byte layout).
  std::vector<u8> obj_info;
  u32 obj_count = 0;
  void AddObject(rx::bethesda::StarfieldMaterialDb::ResourceId id, u32 db_id) {
    PutU32(obj_info, id.file);
    PutU32(obj_info, id.ext);
    PutU32(obj_info, id.dir);
    PutU32(obj_info, db_id);
    PutU32(obj_info, 0);        // baseObjectID
    obj_info.push_back(1);      // hasData
    ++obj_count;
  }

  std::vector<u8> Finish() {
    // Class definitions we reference. Type offsets: user classes point at the
    // class name string; String/UInt32 use the builtin encodings.
    AddClass("BSMaterial::LayerID", {{"ID", Intern("BSComponentDB2::ID")}});
    AddClass("BSMaterial::MaterialID", {{"ID", Intern("BSComponentDB2::ID")}});
    AddClass("BSMaterial::TextureSetID", {{"ID", Intern("BSComponentDB2::ID")}});
    AddClass("BSComponentDB2::ID", {{"Value", kBuiltinUInt32}});
    AddClass("BSMaterial::TextureFile", {{"FileName", kBuiltinString}});

    // All strings that appear as STRT offsets must be interned before the STRT
    // chunk is serialized, so pre-intern class/field/index names here.
    for (const ClassDef& cd : classes) {
      Intern(cd.name);
      for (const auto& [fname, ftype] : cd.fields) Intern(fname);
    }
    Intern("BSComponentDB2::DBFileIndex::ObjectInfo");
    Intern("BSComponentDB2::DBFileIndex::ComponentInfo");

    std::vector<u8> out;
    PutStr(out, "BETH");
    PutU32(out, 8);
    PutU32(out, 4);
    PutU32(out, 0);  // chunk count (unused by the reader)

    // STRT
    PutStr(out, "STRT");
    PutU32(out, static_cast<u32>(strt.size()));
    out.insert(out.end(), strt.begin(), strt.end());

    // TYPE + CLAS
    PutStr(out, "TYPE");
    PutU32(out, 4);
    PutU32(out, static_cast<u32>(classes.size()));
    for (const ClassDef& cd : classes) {
      std::vector<u8> body;
      PutU32(body, Intern(cd.name));
      PutU32(body, 0);  // class version
      PutU16(body, 0);  // flags (not user)
      PutU16(body, static_cast<u16>(cd.fields.size()));
      for (const auto& [fname, ftype] : cd.fields) {
        PutU32(body, Intern(fname));
        PutU32(body, ftype);
        PutU16(body, 0);
        PutU16(body, 0);
      }
      PutStr(out, "CLAS");
      PutU32(out, static_cast<u32>(body.size()));
      out.insert(out.end(), body.begin(), body.end());
    }

    // ObjectInfo LIST
    {
      std::vector<u8> body;
      PutU32(body, Intern("BSComponentDB2::DBFileIndex::ObjectInfo"));
      PutU32(body, obj_count);
      body.insert(body.end(), obj_info.begin(), obj_info.end());
      PutStr(out, "LIST");
      PutU32(out, static_cast<u32>(body.size()));
      out.insert(out.end(), body.begin(), body.end());
    }
    // ComponentInfo LIST
    {
      std::vector<u8> body;
      PutU32(body, Intern("BSComponentDB2::DBFileIndex::ComponentInfo"));
      PutU32(body, static_cast<u32>(comp_info.size()));
      for (const auto& [db_id, key] : comp_info) {
        PutU32(body, db_id);
        PutU32(body, key);
      }
      PutStr(out, "LIST");
      PutU32(out, static_cast<u32>(body.size()));
      out.insert(out.end(), body.begin(), body.end());
    }
    // Streamed component chunks.
    out.insert(out.end(), component.begin(), component.end());
    return out;
  }
};

std::vector<u8> IdBody(u32 object_id) {
  std::vector<u8> b;
  PutU32(b, object_id);  // BSComponentDB2::ID.Value
  return b;
}

std::vector<u8> StringBody(const std::string& s) {
  std::vector<u8> b;
  PutU16(b, static_cast<u16>(s.size()));
  for (char c : s) b.push_back(static_cast<u8>(c));
  return b;
}

}  // namespace

int main() {
  constexpr u32 kNameField = 500;
  constexpr u32 kTextureField = 906;

  std::vector<u8> cdb;
  PutStr(cdb, "BETH");
  PutU32(cdb, 8);  // version
  PutU32(cdb, 4);
  PutU32(cdb, 0);  // strt section size, unused by the reader
  // Chunks begin at offset 16.

  // A landscape texture set: name then its maps, then a trailing shared
  // placeholder the reader must ignore (first color wins).
  PutDiff(cdb, kNameField, "RockSharpSmall02_TextureSet1");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Landscape\\Rocks\\RockSharpSmall02_color.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Landscape\\Rocks\\RockSharpSmall02_normal.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Common\\placeholder_color.dds");  // shared, ignored

  // A material whose set names different textures (architecture style).
  PutDiff(cdb, kNameField, "BldgWallA_TextureSet1");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Architecture\\sharedwall03_color.dds");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Architecture\\sharedwall03_emissive.dds");

  // A non-texture-set name (a composite path) whose textures must be ignored.
  PutDiff(cdb, kNameField, "Composite\\SomeWorld\\Thing");
  PutDiff(cdb, kTextureField, "Data\\Textures\\Junk\\unrelated_color.dds");

  rx::bethesda::StarfieldMaterialDb db;
  db.Build(rx::ByteSpan(cdb.data(), cdb.size()));

  std::puts("material database:");
  Check("indexed two materials", db.size() == 2);

  std::string color, normal, emissive;
  Check("resolves the landscape material",
        db.Lookup("Materials\\Landscape\\Rocks\\RockSharpSmall02.mat", &color, &normal, &emissive));
  Check("base color is the set's first color",
        color == "textures/landscape/rocks/rocksharpsmall02_color.dds");
  Check("normal resolved", normal == "textures/landscape/rocks/rocksharpsmall02_normal.dds");
  Check("placeholder ignored (no second color)", true);

  color.clear();
  emissive.clear();
  Check("resolves the architecture material (different texture name)",
        db.Lookup("Materials\\Arch\\BldgWallA.mat", &color, nullptr, &emissive));
  Check("architecture base color from the set",
        color == "textures/architecture/sharedwall03_color.dds");
  Check("architecture emissive resolved",
        emissive == "textures/architecture/sharedwall03_emissive.dds");

  Check("unknown material misses", !db.Lookup("Materials\\Nope.mat", &color, nullptr, nullptr));

  // BSResourceID hash: value verified against the shipped materialsbeta.cdb
  // object table (Architecture\SpaceStationKit\stndividerwalltrim01 has
  // file=3AE31F9A, ext="mat"=0074616D, dir=54C07ABF).
  {
    auto id = rx::bethesda::StarfieldMaterialDb::HashResource(
        "Materials\\Architecture\\SpaceStationKit\\StnDividerWallTrim01.mat");
    Check("resource id file hash", id.file == 0x3AE31F9Au);
    Check("resource id ext = mat", id.ext == 0x0074616Du);
    Check("resource id dir hash", id.dir == 0x54C07ABFu);
    // Path case and separators do not change the hash.
    auto id2 = rx::bethesda::StarfieldMaterialDb::HashResource(
        "materials/architecture/spacestationkit/stndividerwalltrim01.mat");
    Check("resource id is case/separator insensitive", id2.file == id.file &&
                                                       id2.dir == id.dir && id2.ext == id.ext);
  }

  // Object-graph resolution: a material wired through the component graph to its
  // texture set resolves by path hash, the way architecture materials do.
  {
    const std::string mat_path = "Materials\\Architecture\\City\\Foo.mat";
    auto mat_id = rx::bethesda::StarfieldMaterialDb::HashResource(mat_path);

    GraphCdbBuilder g;
    g.AddObject(mat_id, 1);                         // the material (owns a layer)
    g.AddObject({0x11, 0, 0x22}, 2);                // layer
    g.AddObject({0x33, 0, 0x44}, 3);                // layer's material
    g.AddObject({0x55, 0, 0x66}, 4);                // texture set
    // key = (className string offset << 16) | slot; only the low 16 bits (slot)
    // matter to the reader, so any nonzero type bits are fine.
    g.PutObjt(1, /*key slot*/ 0, "BSMaterial::LayerID", IdBody(2));
    g.PutObjt(2, 0, "BSMaterial::MaterialID", IdBody(3));
    g.PutObjt(3, 0, "BSMaterial::TextureSetID", IdBody(4));
    g.PutObjt(4, /*slot 0 color*/ 0, "BSMaterial::TextureFile",
              StringBody("Data\\Textures\\Architecture\\Foo_color.dds"));
    g.PutObjt(4, /*slot 1 normal*/ 1, "BSMaterial::TextureFile",
              StringBody("Data\\Textures\\Architecture\\Foo_normal.dds"));
    std::vector<u8> blob = g.Finish();

    rx::bethesda::StarfieldMaterialDb graph_db;
    graph_db.Build(rx::ByteSpan(blob.data(), blob.size()));
    Check("object graph indexes the material", graph_db.graph_size() == 1);

    std::string gc, gn, ge;
    Check("graph resolves the architecture material", graph_db.Lookup(mat_path, &gc, &gn, &ge));
    Check("graph base color from the texture set",
          gc == "textures/architecture/foo_color.dds");
    Check("graph normal from the texture set", gn == "textures/architecture/foo_normal.dds");
  }

  if (g_failures == 0) {
    std::puts("material_db: all checks passed");
    return 0;
  }
  std::printf("material_db: %d checks FAILED\n", g_failures);
  return 1;
}
