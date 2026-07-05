#ifndef RECREATION_BETHESDA_FACEGEN_H_
#define RECREATION_BETHESDA_FACEGEN_H_

#include <optional>
#include <string>

#include <base/containers/vector.h>

#include "bethesda/form_id.h"
#include "core/types.h"

namespace rec::bethesda {

class RecordStore;

// Chargen / FaceGen record resolution for Skyrim SE. Turns HDPT, RACE, NPC_ and
// CLFM records into flat structs a head builder can consume: which head parts a
// race or NPC uses, the tri morph files that drive them, the NPC's face-morph
// slider values and tint layers, and the race's tint-layer definitions. Every
// layout here was verified against Skyrim.esm hexdumps (see esminfo hdpt /
// npcface modes); uncertain fields are named `unknown`.

// HDPT PNAM part type.
enum class HeadPartType : u32 {
  kMisc = 0,
  kFace = 1,
  kEyes = 2,
  kHair = 3,
  kFacialHair = 4,
  kScar = 5,
  kEyebrows = 6,
};

// One HDPT NAM0/NAM1 pair: a tri file plus its type marker. NAM0 selects what
// the NAM1 tri is for (race-morph vs chargen-morph); the marker values seen in
// SE are 1 and 2, kept raw here since the head builder keys off the file name.
struct HeadPartTri {
  u32 type = 0;      // NAM0
  std::string path;  // NAM1 vfs tri path
};

// HDPT: one head part (a face, a hairstyle, an eye set, brows, ...).
struct HeadPart {
  GlobalFormId id;
  std::string editor_id;  // EDID
  std::string model;      // MODL nif path
  HeadPartType type = HeadPartType::kMisc;
  u8 flags = 0;  // DATA (bit0 playable, bit1 male, bit2 female, bit4 extra part)
  base::Vector<GlobalFormId> extra_parts;  // HNAM, chained sub-parts (HDPT)
  base::Vector<HeadPartTri> tris;          // NAM0/NAM1 pairs
  GlobalFormId texture_set;                // TNAM (TXST)
  GlobalFormId color;                      // CNAM (CLFM), invalid when absent
  GlobalFormId valid_races;                // RNAM (FLST of allowed races)
};

// CLFM: a color form. Skyrim stores hair and skin colors as CLFM, not COLR;
// CNAM holds the color, FNAM the playable flag.
struct ColorForm {
  GlobalFormId id;
  std::string editor_id;
  u8 rgba[4] = {0, 0, 0, 0};  // CNAM (r,g,b,a bytes)
  bool playable = false;      // FNAM
};

// One NPC_ tint layer: TINI/TINC/TINV/TIAS group. In NPC_ the color is stored
// raw (unlike RACE, where TINC is a CLFM ref), and TINV is a 0-100 integer
// alpha (verified: values cluster 0..100, peak at 100), not the 0-1 float RACE
// uses.
struct NpcTintLayer {
  u16 index = 0;                 // TINI, indexes the race's tint-layer list
  u8 color[4] = {0, 0, 0, 0};    // TINC (r,g,b,a)
  u32 interpolation = 0;         // TINV, 0..100 alpha
  i16 preset = 0;                // TIAS, race tint preset index (-1 = custom)
};

// NAM9 face-morph slider count (verified: 76 bytes / 4 = 19 floats). The first
// 18 are the face-part sliders (~ -1..1); the 19th reads as FLT_MAX on every
// NPC, an unused trailing sentinel, so treat index 18 as junk.
constexpr u32 kFaceMorphCount = 19;

// NPC_: the per-actor face definition.
struct NpcFaceData {
  GlobalFormId id;
  std::string editor_id;                    // EDID
  bool female = false;                       // ACBS flags bit0 (0x1); picks the sex head
  GlobalFormId race;                        // RNAM
  base::Vector<GlobalFormId> head_parts;    // PNAM (HDPT), the chosen parts
  GlobalFormId hair_color;                  // HCLF (CLFM)
  GlobalFormId face_texture_set;            // FTST (TXST), baked-face NPCs only
  f32 skin_tone[3] = {0, 0, 0};             // QNAM (r,g,b floats), texture lighting
  bool has_skin_tone = false;
  f32 face_morph[kFaceMorphCount] = {};     // NAM9 slider values (~ -1..1)
  bool has_face_morph = false;
  // NAMA face-part indices: nose, brows, eyes, mouth (nose/eyes/mouth certain,
  // slot 1 per UESP). Selects which variant of each face-part group is used;
  // -1 means none (slot 1 is -1 on every sampled NPC).
  i32 face_parts[4] = {0, 0, 0, 0};
  bool has_face_parts = false;
  base::Vector<NpcTintLayer> tint_layers;
};

// One RACE tint-layer preset: a CLFM color plus its default weight.
struct RaceTintPreset {
  GlobalFormId color;  // TINC (CLFM ref inside RACE)
  f32 weight = 0;      // TINV (0-1 float inside RACE)
};

// One RACE tint-layer definition (what an NPC's TINI indexes into).
struct RaceTintLayer {
  u16 index = 0;              // TINI
  std::string mask_texture;  // TINT, the layer's mask texture path
  u16 mask_type = 0;         // TINP, which face region the mask paints
  GlobalFormId default_color;  // TIND
  base::Vector<RaceTintPreset> presets;
};

// RACE INDX/HEAD default head part.
struct RaceHeadPart {
  u32 index = 0;           // INDX, part slot
  GlobalFormId head_part;  // HEAD (HDPT)
};

// RACE MPAI/MPAV face-morph availability for one face-part group. The 32-byte
// availability mask is kept raw; no SE consumer decodes its bits yet.
struct RaceMorphAvail {
  u32 index = 0;      // MPAI
  u8 mask[32] = {};   // MPAV
};

// RACE head data for one sex (male under MNAM, female under FNAM, both after
// the NAM0 head-data marker).
struct RaceSexHead {
  base::Vector<RaceHeadPart> parts;             // INDX/HEAD
  base::Vector<RaceMorphAvail> morph_avail;     // MPAI/MPAV
  base::Vector<GlobalFormId> presets;           // RPRM (preset NPC_ forms)
  base::Vector<GlobalFormId> hair_colors;       // AHCM (CLFM)
  base::Vector<GlobalFormId> face_texture_sets; // FTSM (TXST)
  GlobalFormId default_face_texture_set;        // DFTM
  base::Vector<RaceTintLayer> tint_layers;      // TINI/TINT/TINP/TIND + presets
};

// RACE.
struct RaceHeadData {
  GlobalFormId id;
  std::string editor_id;  // EDID
  RaceSexHead male;
  RaceSexHead female;
};

std::optional<HeadPart> ResolveHeadPart(const RecordStore& store, GlobalFormId id);
std::optional<ColorForm> ResolveColorForm(const RecordStore& store, GlobalFormId id);
std::optional<NpcFaceData> ResolveNpcFace(const RecordStore& store, GlobalFormId id);
std::optional<RaceHeadData> ResolveRaceHead(const RecordStore& store, GlobalFormId id);

// Names of the 19 NAM9 sliders in file order, for dumps and UI labels.
const char* FaceMorphName(u32 index);

}  // namespace rec::bethesda

#endif  // RECREATION_BETHESDA_FACEGEN_H_
