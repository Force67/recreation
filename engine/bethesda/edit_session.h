#ifndef RECREATION_BETHESDA_EDIT_SESSION_H_
#define RECREATION_BETHESDA_EDIT_SESSION_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <base/containers/vector.h>

#include "bethesda/form_id.h"
#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/string_writer.h"
#include "core/types.h"

namespace rx::bethesda {

class RawRewriter;

// A form id whose plugin field is kOutputPlugin refers to a record created in
// the session's own output plugin (it has no load order slot yet).
constexpr u16 kOutputPlugin = 0xfffe;

// Which localized string file a text field lives in. The games route each
// localizable subrecord to one of these by field type; the id space is shared
// across all three (each string lives in exactly one file).
enum class StringFile { kStrings, kDlStrings, kIlStrings };

// An editing layer over a loaded RecordStore. It produces an override plugin: a
// new .esp/.esm that masters the games it edits and carries only the records
// the session touched, exactly how a mod patch works.
//
// Two things it does that the flat PluginWriter cannot:
//   - Override / delete existing forms by copying the winning record and
//     re-encoding its form id into the output plugin's master space.
//   - Encode references (Ref) from a load-order-independent GlobalFormId back
//     into a RawFormId, the inverse of LoadOrder::Resolve, adding masters as
//     needed.
//
// Overridden record bodies are kept verbatim: their embedded form ids stay
// valid because the output plugin masters the source plugin with its mod-index
// prefix preserved. Editing an override or authoring a created record with
// SetReference is fully remapped. Output is non-ESL; self-references are encoded
// against the master list as it stands when SetReference is called, so add
// masters / edit overrides before referencing other created forms.
class EditSession {
 public:
  EditSession(const RecordStore& base, const LoadOrder& order, const GameProfile& profile)
      : base_(base), order_(order), profile_(profile) {}

  struct SaveOptions {
    std::string out_file_name;  // written into HEDR bookkeeping context, optional
    std::string author;
    std::string description;
    bool is_master = false;  // ESM flag
    bool is_light = false;   // ESL flag
    bool localized = false;  // ESM string files; also implied by SetLocalizedString
  };

  // Encodes a load-order form id into a raw form id in the output plugin's
  // master space, appending a master if the plugin is not yet referenced.
  // kOutputPlugin encodes as the plugin's own records (self mod-index).
  RawFormId Ref(GlobalFormId id);

  // Marks an existing form as overridden, copying its winning record so its
  // fields can be edited. Returns false if the form is unknown or the source
  // plugin's master order conflicts with masters already committed.
  bool Override(GlobalFormId id);

  // Creates a new record of `type` in the output plugin. Returns its handle
  // (plugin == kOutputPlugin).
  GlobalFormId Create(u32 type);

  // Tombstones a form: emits a deleted override with the same id. (Note: this
  // engine's loader skips deleted records on read, so the base stays winning on
  // reload here; the stub is still correct for the shipping game and tools.)
  bool Remove(GlobalFormId id);

  // Field edits on an overridden or created record (by its handle). Replace or
  // add semantics: an existing field of that type is replaced, else appended.
  bool SetField(GlobalFormId handle, u32 type, ByteSpan bytes);
  bool SetEditorId(GlobalFormId handle, std::string_view editor_id);

  // Sets a localizable text field (e.g. FULL, DESC) on a localized plugin: the
  // text is interned into the string table for `file` and the field stores the
  // assigned u32 string id instead of inline text. Marks the output localized;
  // Save then writes the strings/<plugin>_<language>.* files and sets the
  // localized flag. The id space is shared across the three files.
  bool SetLocalizedString(GlobalFormId handle, u32 field_type, std::string_view text,
                          StringFile file = StringFile::kStrings);
  // Writes a single form-id field, encoding `target` with Ref.
  bool SetReference(GlobalFormId handle, u32 field_type, GlobalFormId target);
  // Appends a form-id field (does not replace), for repeated reference fields
  // like FLST's LNAM.
  bool AddReference(GlobalFormId handle, u32 field_type, GlobalFormId target);
  bool RemoveField(GlobalFormId handle, u32 type);

  // Nested content. The parent (cell / dialogue) and each child must already be
  // Create'd or Override'd in this session. Children are emitted inside the
  // parent's GRUP subtree instead of as flat top level records.

  // Places a placed reference (REFR / ACHR) inside an interior cell, in the
  // cell's persistent or temporary children group.
  bool PlaceInInteriorCell(GlobalFormId cell, GlobalFormId reference, bool persistent = false);

  // Adds an INFO response under a DIAL topic, in evaluation order.
  bool AddTopicInfo(GlobalFormId dialogue, GlobalFormId info);

  // Places a reference inside an exterior cell of a worldspace. The cell must
  // carry an XCLC grid field (SetField with the cell's grid coordinates); the
  // block/sub-block groups are derived from it.
  bool PlaceInExteriorCell(GlobalFormId worldspace, GlobalFormId cell,
                           GlobalFormId reference, bool persistent = false);

  // Serializes the override plugin to disk.
  bool Save(const std::string& path, const SaveOptions& options);

  // In-place editing bridge. Targets a single already-loaded plugin so form ids
  // resolve against that plugin's own fixed master list (no new masters, no
  // remap), instead of building a patch. Then ApplyEditsTo feeds each edited
  // record of that plugin to a RawRewriter as a Replace (or Delete), so an
  // existing plugin can be modified byte-faithfully with only the touched
  // records changed. Created (brand new) records are skipped by the bridge --
  // use Save for those.
  bool SetInPlaceTarget(u16 plugin_index);
  bool ApplyEditsTo(RawRewriter& rewriter);

  const base::Vector<std::string>& masters() const { return masters_; }
  size_t edit_count() const { return order_of_entries_.size(); }

 private:
  struct Entry {
    Record record;                            // owns decompressed + subrecords
    base::Vector<base::Vector<u8>> storage;   // owns edited field bytes
    GlobalFormId handle;
    bool deleted = false;
  };

  struct CellChildren {
    base::Vector<GlobalFormId> persistent;
    base::Vector<GlobalFormId> temporary;
  };

  Entry* FindEntry(GlobalFormId handle);
  Entry& NewEntry(GlobalFormId handle);
  u16 AddMasterName(const std::string& name);  // index of name, appending if new
  bool RequireChain(u16 plugin);               // ensure the plugin's master prefix
  ByteSpan Store(Entry* entry, const u8* data, size_t size);
  bool PutField(GlobalFormId handle, u32 type, ByteSpan bytes, bool replace);
  Record BuildOutput(Entry& entry);              // header re-encoded, body copied
  void EncodeEntry(GlobalFormId handle, base::Vector<u8>* out, u32* count);
  void EncodeCellChildren(GlobalFormId cell, base::Vector<u8>* out, u32* count);
  base::Vector<u8> BuildCellGroup(u32* count);   // nested CELL top group, or empty
  base::Vector<u8> BuildDialGroup(u32* count);   // nested DIAL top group, or empty
  base::Vector<u8> BuildWorldGroup(u32* count);  // nested WRLD top group, or empty
  bool WriteStringFiles(const std::string& plugin_path);  // the strings/*.* files

  const RecordStore& base_;
  const LoadOrder& order_;
  const GameProfile& profile_;
  base::Vector<std::string> masters_;
  std::unordered_map<u64, Entry> entries_;
  base::Vector<u64> order_of_entries_;  // deterministic emit order
  u32 next_local_id_ = 0x800;

  // Nested content: parents keep their child lists; children are marked claimed
  // so the flat pass skips them.
  std::unordered_map<u64, CellChildren> cell_children_;
  base::Vector<u64> cell_order_;
  std::unordered_map<u64, base::Vector<GlobalFormId>> topic_infos_;
  base::Vector<u64> dial_order_;
  std::unordered_map<u64, base::Vector<GlobalFormId>> world_cells_;  // worldspace -> cells
  base::Vector<u64> world_order_;
  std::unordered_set<u64> claimed_;

  // Localized string tables (shared id space across the three files).
  StringTableWriter strings_;
  StringTableWriter dlstrings_;
  StringTableWriter ilstrings_;
  u32 next_string_id_ = 1;  // 0 is reserved by the format
  bool localized_ = false;

  // In-place bridge target: when set, Ref resolves against this plugin's fixed
  // master list instead of the growing patch list.
  bool in_place_ = false;
  u16 in_place_plugin_ = 0xffff;
  base::Vector<std::string> in_place_masters_;
};

}  // namespace rx::bethesda

#endif  // RECREATION_BETHESDA_EDIT_SESSION_H_
