#ifndef RECREATION_BETHESDA_FORM_ID_H_
#define RECREATION_BETHESDA_FORM_ID_H_

#include "core/types.h"

namespace rx::bethesda {

// Raw form id as stored in a plugin. The top byte indexes into that plugin's
// master list, 0xFE marks an ESL slot where the next 12 bits select the
// light plugin and only 12 bits remain for the record.
struct RawFormId {
  u32 value = 0;

  u8 mod_index() const { return static_cast<u8>(value >> 24); }
  bool is_esl_slot() const { return mod_index() == 0xfe; }
  u16 esl_index() const { return static_cast<u16>((value >> 12) & 0xfff); }
  u32 local_id() const { return is_esl_slot() ? value & 0xfff : value & 0xffffff; }
};

// Load order independent identity: which plugin defined the record plus its
// local id. Stable across save games and differing mod lists.
struct GlobalFormId {
  u16 plugin = 0xffff;  // index into LoadOrder
  u32 local_id = 0;

  bool operator==(const GlobalFormId&) const = default;
  u64 packed() const { return static_cast<u64>(plugin) << 32 | local_id; }
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_FORM_ID_H_
