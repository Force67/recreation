#ifndef RECREATION_BETHESDA_TES3_H_
#define RECREATION_BETHESDA_TES3_H_

#include <base/containers/vector.h>

#include "bethesda/plugin.h"
#include "bethesda/record.h"
#include "core/types.h"

namespace rx::bethesda {

// Classic Morrowind plugins are flat TES3 records: 16 byte headers, no GRUP
// groups, u32-sized subrecords, references bound by string id and inlined in
// their CELL record. Rather than teach every downstream consumer that model,
// the translator rewrites the file into the modern shape at load:
//
//   - every referenced base record gets a synthetic form id (string id -> id),
//   - a synthetic "Vvardenfell" WRLD stands in for the implicit worldspace,
//   - each 8192-unit exterior cell splits into 2x2 virtual 4096-unit cells
//     (the 65x65 LAND grid shares the modern 128-unit vertex spacing, so each
//     quadrant re-encodes exactly into a 33x33 VHGT/VNML/VCLR),
//   - inline cell references become REFR records binned by position,
//   - the VTEX texture grid becomes BTXT/ATXT+VTXT layers over synthesized
//     LTEX/TXST pairs naming the real texture files.
//
// The result walks and parses exactly like a Skyrim-era plugin.
struct Tes3Translation {
  struct Rec {
    RecordHeader header;
    u32 payload_offset = 0;  // into arena
    u32 payload_size = 0;
    GroupContext ctx;
  };
  base::Vector<u8> arena;  // synthesized payloads, modern subrecord encoding
  base::Vector<Rec> records;
  f32 version = 0;
  u32 record_count = 0;  // HEDR record count of the source file
};

bool TranslateTes3(ByteSpan data, Tes3Translation* out);

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_TES3_H_
