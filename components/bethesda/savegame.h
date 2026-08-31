#ifndef COMPONENTS_BETHESDA_SAVEGAME_H
#define COMPONENTS_BETHESDA_SAVEGAME_H

// Reading a Bethesda savegame and turning it back into live world state.
//
// The layering here is deliberate, because the three jobs fail differently and
// want testing separately:
//
//   1. SaveFile   parses the CONTAINER: header, plugin list, file location
//                 table, globals, the ChangeForm block, the form-id map. Knows
//                 nothing about what a ChangeForm means.
//   2. ChangeForm decoding turns one record's versioned, type-specific,
//                 optionally-compressed payload into a typed struct.
//   3. Applying   writes those structs onto the live world (ECS, quests,
//                 inventories, the player).
//
// GAME-AGNOSTIC BY CONSTRUCTION. Skyrim LE/SE and Fallout 4/76 share this
// shape and differ in header magic, compression, and which ChangeForm types
// carry which fields. `Game` selects those; nothing above layer 1 should branch
// on the magic string.

#include <base/containers/pair.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include "components/bethesda/savegame_papyrus.h"
#include "core/types.h"

namespace rx::bethesda {

enum class SaveFormat {
  kUnknown,
  kSkyrimLe,   // "TESV_SAVEGAME", zlib
  kSkyrimSe,   // "TESV_SAVEGAME", header version 12+, LZ4 frame
  kFallout4,   // "FO4_SAVEGAME"
};

// A ChangeForm's record type. The values are Skyrim's on-disk enumeration, so
// do not renumber: each one was confirmed by resolving every change form in a
// Skyrim SE save through the form-id map and reading back the record's real
// signature from the masters, so they are measured, not inferred from the
// record type order. Fallout 4 numbers the same types differently and layer 1
// translates (see ChangeFormTypeOf), which is what keeps everything above it
// off the game.
enum class ChangeFormType : u8 {
  kRefr = 0, kAchr = 1, kPgre = 3, kCell = 6, kInfo = 7, kQust = 8,
  kNpc = 9, kBook = 13, kIngr = 16, kEczn = 29, kFact = 31, kWoop = 34,
  kSmqn = 36, kScen = 37, kLctn = 38, kRela = 39, kPhzd = 40, kFlst = 43,
  kLvln = 44, kLvli = 45, kEnch = 48, kInnr = 50, kUnknown = 63,
};

// The record type behind a change form's raw type byte (its low 6 bits).
//
// Fallout 4 has no INGR, so every type from there up sits one below Skyrim's:
// measured over 226623 change forms of three Fallout 4 saves by resolving each
// one's id in Fallout4.esm and reading the signature back, which puts ECZN at
// 28 (Skyrim 29), FACT at 30 (31), SMQN 35 (36), SCEN 36 (37), LCTN 37 (38),
// PHZD 39 (40), FLST 42 (43), LVLI 44 (45) and INNR, which Skyrim does not
// have, at 49. Where exactly the shift starts is not observable: no Fallout 4
// save in hand carries a change form whose type falls in 17..27.
ChangeFormType ChangeFormTypeOf(SaveFormat format, u8 type_byte);

// What a form the player made at runtime was made as. The four tables are
// stored in this order and nothing else in the save says which kind an id is.
enum class CreatedFormKind : u8 {
  kWeaponEnchantment,
  kArmorEnchantment,
  kPotion,
  kPoison,
};

struct CreatedEffect {
  u32 effect = 0;        // MGEF form id, already resolved through the save
  f32 magnitude = 0.0f;
  u32 duration = 0;      // seconds
  // The price the game computed, which is what the item is worth. Only the
  // first effect of a multi-effect item carries it; the rest read 0.
  f32 value = 0.0f;
};

// A base form the save invented: a player enchantment or an alchemy product.
// Its id is 0xFFxxxxxx and no plugin has a record for it, so this table is the
// only description of it that exists.
struct CreatedForm {
  u32 form_id = 0;
  CreatedFormKind kind = CreatedFormKind::kWeaponEnchantment;
  // Never zero and small (1..24 across the reference save), which reads like a
  // copy count; ReSaver calls it timesUsed. Nothing in the file settles it.
  u32 unknown_count = 0;
  base::Vector<CreatedEffect> effects;
};

// One row of the game's Stats page. The name is the game's own key, stored
// untranslated and in full ("Locations Discovered"), so it is the only label
// there is; `category` is the tab it sits under. Measured over the reference
// save: 108 rows in 7 categories, the whole table consumed exactly.
struct MiscStat {
  base::String name;
  // The Stats page tab. Skyrim numbers them 0..6 and Fallout 4 uses 0..5 and 7,
  // so the number is the game's own and there is no shared naming to give it.
  u8 category = 0;
  u32 value = 0;
};

// Where the game itself believes the player is. This is kept beside the
// player's reference, not derived from it, and the two do not say quite the
// same thing (see cell_x/cell_y).
struct SavedPlayerLocation {
  bool valid = false;
  // The next id the game would hand a form it creates. Every 0xFFxxxxxx id in
  // the save is below it.
  u32 next_object_id = 0;
  // The worldspace cell_x/cell_y count in, 0 when the save has none.
  u32 coord_worldspace = 0;
  // NOT the cell containing `position`. Measured across 55 saves (the Skyrim
  // reference plus 54 Fallout 4): it equals floor(position/cell_size) in 38 of
  // them and is off by exactly one cell in one or both axes in the rest, never
  // by more, which is a grid centre the game updates as it streams rather than
  // the player's own cell. `position` is what places the player.
  i32 cell_x = 0, cell_y = 0;
  // The worldspace the player stands in, or the cell when that is an interior.
  // In the reference save this is the same form the player's own ACHR names as
  // its parent, and `position` matches the ACHR's transform byte for byte.
  u32 parent = 0;
  f32 position[3] = {};
};

// The sky as the save left it, so resuming does not roll a fresh one.
struct SavedWeather {
  bool valid = false;
  u32 climate = 0;
  // The weather in force. When `previous` is set the sky is mid-fade from it
  // into this one and `transition` says how far; the reference save is settled
  // (previous 0, transition 1.0), so the mid-fade reading is inferred from the
  // field's position and not confirmed against a save that was fading.
  u32 weather = 0;
  u32 previous = 0;
  // The weather the active REGN region asks for, 0 when the worldspace climate
  // is in charge.
  u32 region_weather = 0;
  f32 current_time = 0.0f;  // game hours
  f32 begin_time = 0.0f;    // game hours the current weather started at
  f32 transition = 0.0f;    // 0 = just started, 1 = fully arrived
};

// Two ingredients the game recorded together. NOT an ingredient and an effect:
// every one of the 20 refids in the reference save resolves to an INGR record,
// and no pair of them shares a single magic effect, so this is a memory of
// combinations that produced nothing rather than the discovered-effect set
// UESP describes. Nothing in the file says which of the two came first.
struct IngredientPair {
  u32 first = 0;
  u32 second = 0;
};

// One entry of the favourites menu: a spell or shout the player put there, and
// the number key it is bound to, or -1 when it sits in the list unbound.
struct MagicFavourite {
  u32 form_id = 0;
  i32 hotkey = -1;
};

// One record as it sits in the file: still opaque, already decompressed.
struct ChangeForm {
  u32 form_id = 0;          // resolved through the form-id map, not raw
  ChangeFormType type = ChangeFormType::kUnknown;
  u32 flags = 0;            // which optional field groups the payload carries
  u16 version = 0;
  base::Vector<u8> data;    // decompressed payload
};

// The container. Parsing this must never require a loaded game.
struct SaveFile {
  SaveFormat format = SaveFormat::kUnknown;
  u32 save_number = 0;
  base::String player_name;
  u32 player_level = 0;
  base::String player_location;   // cell/worldspace display name at save time
  base::String game_time;
  f32 in_game_seconds = 0.0f;

  // The save's own load order. A form id in the file is only meaningful
  // against this list, so applying a save to a different order must remap or
  // refuse rather than silently land on the wrong records.
  base::Vector<base::String> plugins;
  // Light (ESL) plugins, kept apart because they do not occupy a load order
  // slot: their forms live in the 0xFE000000 range, not at index plugins.size().
  base::Vector<base::String> light_plugins;

  // The form-id map every RefID in the file indexes into, kept because a
  // ChangeForm payload embeds RefIDs too and layer 2 cannot resolve them alone.
  base::Vector<u32> form_ids;

  base::Vector<ChangeForm> change_forms;
  // Global variables (form id -> value), the cheapest thing a save carries and
  // the one most gameplay systems read.
  base::Vector<base::Pair<u32, f32>> globals;
  // Forms the save itself created. Not one of them has a ChangeForm of its own,
  // so this list is their whole definition and a 0xFFxxxxxx id naming one
  // cannot be resolved out of the change form block at all.
  base::Vector<CreatedForm> created_forms;
  // Every live script instance and the value of its member variables. Empty
  // when the save has no Papyrus table or its layout did not walk (see
  // savegame_papyrus.h); a partly read heap is never handed back.
  PapyrusHeap papyrus;

  // The rest of the global data, in the order a player notices it. Each one is
  // left empty rather than failing the whole file when its walk does not come
  // out even, because a save the engine already boots from must keep booting
  // if a later game moves a field.
  base::Vector<MiscStat> misc_stats;
  SavedPlayerLocation player_place;
  SavedWeather weather;
  base::Vector<IngredientPair> ingredient_pairs;

  // The favourites menu, in the order the save lists it.
  base::Vector<MagicFavourite> magic_favourites;

  // The last weapon, spell and shout the player used, 0 for none. The shout is
  // what the game has selected: nothing else in the file says which one is up.
  u32 last_used_weapon = 0;
  u32 last_used_spell = 0;
  u32 last_used_shout = 0;
};

// Layer 1. Returns false and leaves `out` untouched on a malformed file.
bool ReadSaveFile(ByteSpan bytes, SaveFile& out);

// Sniffs the format from the magic and header version without a full parse.
SaveFormat DetectSaveFormat(ByteSpan bytes);

}  // namespace rx::bethesda

#endif  // COMPONENTS_BETHESDA_SAVEGAME_H
