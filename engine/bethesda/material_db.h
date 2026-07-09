#ifndef RECREATION_BETHESDA_MATERIAL_DB_H_
#define RECREATION_BETHESDA_MATERIAL_DB_H_

#include <string>
#include <string_view>
#include <unordered_map>

#include "core/types.h"

namespace rx::bethesda {

// Starfield ships its materials compiled into one database, materialsbeta.cdb
// (a BSComponentDB2). A material (.mat) resolves its textures through that
// database, not by a path convention, so architecture and ship materials carry
// no usable path-mirror textures on their own.
//
// The database serializes each TextureSet object as its name (a string field,
// component-type id 500) immediately followed by that set's texture file paths
// (component-type id 906), so a linear pass recovers, for every material whose
// TextureSet is named "<material>_TextureSet<n>", its base-color, normal and
// emissive maps. That covers the bulk of materials (~73%); the rest reach their
// textures through layer/edge references this reader does not follow, and fall
// back to the path convention or gray. The first texture of each kind in a set
// is the set's own; a shared placeholder trails the group and is ignored.
class StarfieldMaterialDb {
 public:
  // Parses a materialsbeta.cdb blob. The blob is not retained. Calling with an
  // empty span leaves the database empty (every Lookup misses).
  void Build(ByteSpan cdb);

  // Resolves the base-color, normal and emissive texture vfs paths
  // ("textures/...dds") for a material path ("Materials\X\Y.mat"). Outputs are
  // left untouched when a map is absent. Returns false when the material's
  // TextureSet is not in the database.
  bool Lookup(std::string_view mat_path, std::string* base_color, std::string* normal,
              std::string* emissive) const;

  bool empty() const { return by_stem_.empty(); }
  size_t size() const { return by_stem_.size(); }

 private:
  struct Textures {
    std::string base_color;
    std::string normal;
    std::string emissive;
  };
  // material stem (lower case file name of the .mat, no extension) -> textures.
  std::unordered_map<std::string, Textures> by_stem_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_MATERIAL_DB_H_
