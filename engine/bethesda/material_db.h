#ifndef RECREATION_BETHESDA_MATERIAL_DB_H_
#define RECREATION_BETHESDA_MATERIAL_DB_H_

#include <string>
#include <string_view>
#include <unordered_map>

#include "core/types.h"

namespace rx::bethesda {

// Starfield ships its materials compiled into one database, materialsbeta.cdb
// (a BSComponentDB2). A material (.mat) resolves its textures through that
// database, not by a path convention.
//
// The reader parses the BSComponentDB2 object graph: an object table keyed by
// BSResourceID (the .mat path CRC), per-object components, and the ID edges that
// link them, then follows Material -> layer -> material -> TextureSet by object
// id to reach the base-color/normal/emissive texture paths. This resolves
// architecture and ship materials, which link their textures by id rather than
// by a "<material>_TextureSet<n>" name. A linear TextureSet-name scan is kept as
// a fallback for materials the graph does not reach (mostly landscape), so net
// coverage only grows.
class StarfieldMaterialDb {
 public:
  // Parses a materialsbeta.cdb blob. The blob is not retained. Calling with an
  // empty span leaves the database empty (every Lookup misses).
  void Build(ByteSpan cdb);

  // Resolves the base-color, normal and emissive texture vfs paths
  // ("textures/...dds") for a material path ("Materials\X\Y.mat"). Outputs are
  // left untouched when a map is absent. Returns false when the material is not
  // in the database.
  bool Lookup(std::string_view mat_path, std::string* base_color, std::string* normal,
              std::string* emissive) const;

  bool empty() const { return by_resource_.empty() && by_stem_.empty(); }
  size_t size() const { return by_resource_.size() + by_stem_.size(); }
  // Materials resolved through the object graph (architecture/ships included).
  size_t graph_size() const { return by_resource_.size(); }

  // A Starfield resource id: CRC of the directory and base name plus the packed
  // extension, matching the path hash the compiled database keys objects by.
  struct ResourceId {
    u32 file = 0;
    u32 ext = 0;
    u32 dir = 0;
    bool operator==(const ResourceId& r) const {
      return file == r.file && ext == r.ext && dir == r.dir;
    }
  };
  static ResourceId HashResource(std::string_view path);

 private:
  struct Textures {
    std::string base_color;
    std::string normal;
    std::string emissive;
  };
  struct ResourceIdHash {
    size_t operator()(const ResourceId& id) const {
      return (static_cast<u64>(id.file) << 32) ^ (static_cast<u64>(id.dir) << 8) ^ id.ext;
    }
  };

  void BuildGraphIndex(ByteSpan cdb);
  void BuildStemIndex(ByteSpan cdb);

  // material path hash -> textures (object-graph resolution, primary).
  std::unordered_map<ResourceId, Textures, ResourceIdHash> by_resource_;
  // material stem (lower case file name of the .mat, no extension) -> textures
  // (TextureSet-name scan, fallback).
  std::unordered_map<std::string, Textures> by_stem_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_MATERIAL_DB_H_
