#include "bethesda/edit_session.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>

#include "bethesda/plugin.h"
#include "bethesda/raw_rewriter.h"
#include "bethesda/writer.h"
#include "core/log.h"

namespace rec::bethesda {
namespace {

constexpr u32 kEdid = FourCc('E', 'D', 'I', 'D');
constexpr u32 kCell = FourCc('C', 'E', 'L', 'L');
constexpr u32 kDial = FourCc('D', 'I', 'A', 'L');
constexpr u32 kWrld = FourCc('W', 'R', 'L', 'D');
constexpr u32 kXclc = FourCc('X', 'C', 'L', 'C');

// GRUP group types (TES4 file format).
constexpr i32 kGroupTop = 0;
constexpr i32 kGroupWorldChildren = 1;
constexpr i32 kGroupInteriorBlock = 2;
constexpr i32 kGroupInteriorSubBlock = 3;
constexpr i32 kGroupExteriorBlock = 4;
constexpr i32 kGroupExteriorSubBlock = 5;
constexpr i32 kGroupCellChildren = 6;
constexpr i32 kGroupTopicChildren = 7;
constexpr i32 kGroupCellPersistent = 8;
constexpr i32 kGroupCellTemporary = 9;

i32 FloorDiv(i32 a, i32 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Packs a pair of grid coordinates into an exterior CELL block/sub-block group
// label the way the games (and xEdit) do: X in the high word, Y in the low word,
// each a signed int16. Matches xEdit's wbGridCellToGroupLabel:
//   Result := Word(y) or (Word(x) shl 16).
// This engine's own loader keys exteriors off the CELL's XCLC and the children
// group labels, not these numbers -- the packing exists purely so the output is
// valid for the real games and displays correctly in xEdit/CK.
u32 PackGrid(i32 x, i32 y) {
  return (static_cast<u32>(static_cast<u16>(x)) << 16) | static_cast<u16>(y);
}

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

u16 EditSession::AddMasterName(const std::string& name) {
  for (u16 i = 0; i < masters_.size(); ++i) {
    if (IEquals(masters_[i], name)) return i;
  }
  masters_.push_back(name);
  return static_cast<u16>(masters_.size() - 1);
}

bool EditSession::RequireChain(u16 plugin) {
  const PluginFile* pf = base_.PluginAt(plugin);
  if (!pf) {
    REC_ERROR("edit: source plugin {} not loaded", plugin);
    return false;
  }
  // The output plugin must begin with this plugin's masters followed by the
  // plugin itself, so the mod-index prefix the verbatim body relies on is
  // preserved.
  base::Vector<std::string> chain;
  for (const std::string& m : pf->masters()) chain.push_back(m);
  chain.push_back(order_.plugins()[plugin]);

  for (u16 k = 0; k < chain.size(); ++k) {
    if (k < masters_.size()) {
      if (!IEquals(masters_[k], chain[k])) {
        REC_ERROR("edit: master order conflict at {} ({} vs {})", k, masters_[k].c_str(),
                  chain[k].c_str());
        return false;
      }
    } else {
      masters_.push_back(chain[k]);
    }
  }
  return true;
}

RawFormId EditSession::Ref(GlobalFormId id) {
  u32 local = id.local_id & 0xffffff;
  u16 mod_index;
  if (in_place_) {
    // Resolve against the target plugin's fixed master list: its own forms (and
    // created handles) use the self index; references use the master's index.
    if (id.plugin == kOutputPlugin || id.plugin == in_place_plugin_) {
      mod_index = static_cast<u16>(in_place_masters_.size());
    } else {
      const std::string& name = order_.plugins()[id.plugin];
      mod_index = static_cast<u16>(in_place_masters_.size());  // fallback: self
      for (u16 i = 0; i < in_place_masters_.size(); ++i) {
        if (IEquals(in_place_masters_[i], name)) {
          mod_index = i;
          break;
        }
      }
      if (mod_index == in_place_masters_.size()) {
        REC_ERROR("edit: in-place reference to non-master plugin {} would need a new master", name);
      }
    }
  } else if (id.plugin == kOutputPlugin) {
    mod_index = static_cast<u16>(masters_.size());  // the output plugin's own space
  } else {
    mod_index = AddMasterName(order_.plugins()[id.plugin]);
  }
  return RawFormId{(static_cast<u32>(mod_index) << 24) | local};
}

bool EditSession::SetInPlaceTarget(u16 plugin_index) {
  const PluginFile* pf = base_.PluginAt(plugin_index);
  if (!pf) {
    REC_ERROR("edit: in-place target plugin {} not loaded", plugin_index);
    return false;
  }
  in_place_ = true;
  in_place_plugin_ = plugin_index;
  in_place_masters_.clear();
  for (const std::string& m : pf->masters()) in_place_masters_.push_back(m);

  // New records inserted in place must not collide with the plugin's existing
  // forms, so start allocating above its highest self-defined local id.
  const u8 self_index = static_cast<u8>(in_place_masters_.size());
  u32 max_local = next_local_id_ - 1;
  pf->VisitRecordsRaw([&](const RecordHeader& header, ByteSpan, const GroupContext&) {
    RawFormId id = header.form_id;
    if (!id.is_esl_slot() && id.mod_index() == self_index && id.local_id() > max_local) {
      max_local = id.local_id();
    }
  });
  next_local_id_ = max_local + 1;
  return true;
}

bool EditSession::ApplyEditsTo(RawRewriter& rewriter) {
  if (!in_place_) {
    REC_ERROR("edit: ApplyEditsTo requires SetInPlaceTarget");
    return false;
  }
  for (u64 packed : order_of_entries_) {
    Entry& entry = entries_[packed];
    if (entry.handle.plugin == kOutputPlugin) {
      // A brand new record: insert it into its top-level type group. Nested
      // (placed) new records can't be spliced into an existing tree in place.
      if (claimed_.count(packed)) {
        REC_WARN("edit: in-place insert skips nested created record {:06x}", entry.handle.local_id);
        continue;
      }
      Record out = BuildOutput(entry);
      base::Vector<u8> encoded;
      EncodeRecord(out, &encoded);
      rewriter.Insert(out.header.type, std::move(encoded));
      continue;
    }
    // Only records that belong to the target plugin can be substituted in place.
    if (entry.handle.plugin != in_place_plugin_) continue;
    u32 raw = Ref(entry.handle).value;
    if (entry.deleted) {
      rewriter.Delete(raw);
      continue;
    }
    Record out = BuildOutput(entry);
    // Preserve the record's original compression choice for fidelity.
    bool compress = (entry.record.header.flags & kRecordFlagCompressed) != 0;
    base::Vector<u8> encoded;
    EncodeRecord(out, &encoded, compress);
    rewriter.Replace(raw, std::move(encoded));
  }
  return true;
}

EditSession::Entry* EditSession::FindEntry(GlobalFormId handle) {
  auto it = entries_.find(handle.packed());
  return it == entries_.end() ? nullptr : &it->second;
}

EditSession::Entry& EditSession::NewEntry(GlobalFormId handle) {
  order_of_entries_.push_back(handle.packed());
  Entry& entry = entries_[handle.packed()];
  entry.handle = handle;
  return entry;
}

bool EditSession::Override(GlobalFormId id) {
  if (FindEntry(id)) return true;  // already overriding
  const RecordStore::StoredRecord* stored = base_.Find(id);
  if (!stored) {
    REC_ERROR("edit: cannot override unknown form {:04x}:{:06x}", id.plugin, id.local_id);
    return false;
  }
  if (!RequireChain(stored->winning_plugin)) return false;
  Entry& entry = NewEntry(id);
  if (!ParseRecordPayload(stored->header, stored->payload, &entry.record)) {
    REC_ERROR("edit: failed to parse form {:04x}:{:06x} for override", id.plugin, id.local_id);
    return false;
  }
  return true;
}

GlobalFormId EditSession::Create(u32 type) {
  GlobalFormId handle{kOutputPlugin, next_local_id_++};
  Entry& entry = NewEntry(handle);
  entry.record.header.type = type;
  return handle;
}

bool EditSession::Remove(GlobalFormId id) {
  const RecordStore::StoredRecord* stored = base_.Find(id);
  if (!stored) return false;
  if (!RequireChain(stored->winning_plugin)) return false;
  Entry* entry = FindEntry(id);
  if (!entry) entry = &NewEntry(id);
  entry->record.header.type = stored->header.type;
  entry->deleted = true;
  return true;
}

ByteSpan EditSession::Store(Entry* entry, const u8* data, size_t size) {
  base::Vector<u8>& buffer = entry->storage.emplace_back();
  buffer.insert(buffer.end(), data, data + size);
  return ByteSpan(buffer.data(), buffer.size());
}

bool EditSession::PutField(GlobalFormId handle, u32 type, ByteSpan bytes, bool replace) {
  Entry* entry = FindEntry(handle);
  if (!entry) {
    REC_ERROR("edit: no record for handle {:04x}:{:06x}", handle.plugin, handle.local_id);
    return false;
  }
  ByteSpan owned = Store(entry, bytes.data(), bytes.size());
  if (replace) {
    for (Subrecord& sub : entry->record.subrecords) {
      if (sub.type == type) {
        sub.data = owned;
        return true;
      }
    }
  }
  entry->record.subrecords.push_back(Subrecord{type, owned});
  return true;
}

bool EditSession::SetField(GlobalFormId handle, u32 type, ByteSpan bytes) {
  return PutField(handle, type, bytes, /*replace=*/true);
}

bool EditSession::SetEditorId(GlobalFormId handle, std::string_view editor_id) {
  base::Vector<u8> buffer;
  buffer.insert(buffer.end(), editor_id.begin(), editor_id.end());
  buffer.push_back(0);
  return SetField(handle, kEdid, ByteSpan(buffer.data(), buffer.size()));
}

bool EditSession::SetLocalizedString(GlobalFormId handle, u32 field_type, std::string_view text,
                                     StringFile file) {
  u32 id = next_string_id_++;
  StringTableWriter& table = file == StringFile::kStrings     ? strings_
                             : file == StringFile::kDlStrings ? dlstrings_
                                                              : ilstrings_;
  table.Set(id, text);
  localized_ = true;
  // The field carries the u32 string id instead of the text.
  return SetField(handle, field_type, ByteSpan(reinterpret_cast<const u8*>(&id), 4));
}

bool EditSession::SetReference(GlobalFormId handle, u32 field_type, GlobalFormId target) {
  RawFormId raw = Ref(target);
  return PutField(handle, field_type, ByteSpan(reinterpret_cast<const u8*>(&raw.value), 4),
                  /*replace=*/true);
}

bool EditSession::AddReference(GlobalFormId handle, u32 field_type, GlobalFormId target) {
  RawFormId raw = Ref(target);
  return PutField(handle, field_type, ByteSpan(reinterpret_cast<const u8*>(&raw.value), 4),
                  /*replace=*/false);
}

bool EditSession::RemoveField(GlobalFormId handle, u32 type) {
  Entry* entry = FindEntry(handle);
  if (!entry) return false;
  auto& subs = entry->record.subrecords;
  for (Subrecord* it = subs.begin(); it != subs.end(); ++it) {
    if (it->type == type) {
      subs.erase(it);
      return true;
    }
  }
  return false;
}

bool EditSession::PlaceInInteriorCell(GlobalFormId cell, GlobalFormId reference,
                                      bool persistent) {
  if (!FindEntry(cell) || !FindEntry(reference)) {
    REC_ERROR("edit: cell and reference must be created or overridden before placement");
    return false;
  }
  auto it = cell_children_.find(cell.packed());
  if (it == cell_children_.end()) cell_order_.push_back(cell.packed());
  CellChildren& children = cell_children_[cell.packed()];
  (persistent ? children.persistent : children.temporary).push_back(reference);
  claimed_.insert(cell.packed());
  claimed_.insert(reference.packed());
  return true;
}

bool EditSession::AddTopicInfo(GlobalFormId dialogue, GlobalFormId info) {
  if (!FindEntry(dialogue) || !FindEntry(info)) {
    REC_ERROR("edit: dialogue and info must be created or overridden before linking");
    return false;
  }
  auto it = topic_infos_.find(dialogue.packed());
  if (it == topic_infos_.end()) dial_order_.push_back(dialogue.packed());
  topic_infos_[dialogue.packed()].push_back(info);
  claimed_.insert(dialogue.packed());
  claimed_.insert(info.packed());
  return true;
}

bool EditSession::PlaceInExteriorCell(GlobalFormId worldspace, GlobalFormId cell,
                                      GlobalFormId reference, bool persistent) {
  if (!FindEntry(worldspace) || !FindEntry(cell) || !FindEntry(reference)) {
    REC_ERROR("edit: worldspace, cell and reference must all exist before placement");
    return false;
  }
  auto it = world_cells_.find(worldspace.packed());
  if (it == world_cells_.end()) world_order_.push_back(worldspace.packed());
  base::Vector<GlobalFormId>& cells = world_cells_[worldspace.packed()];
  if (!cells.Contains(cell)) cells.push_back(cell);

  CellChildren& children = cell_children_[cell.packed()];
  (persistent ? children.persistent : children.temporary).push_back(reference);
  claimed_.insert(worldspace.packed());
  claimed_.insert(cell.packed());
  claimed_.insert(reference.packed());
  return true;
}

Record EditSession::BuildOutput(Entry& entry) {
  Record out;
  out.header = entry.record.header;
  out.header.form_id = Ref(entry.handle);
  if (entry.deleted) {
    out.header.flags |= kRecordFlagDeleted;
  } else {
    out.subrecords = entry.record.subrecords;  // spans stay valid (owned by entry)
  }
  return out;
}

void EditSession::EncodeEntry(GlobalFormId handle, base::Vector<u8>* out, u32* count) {
  Entry* entry = FindEntry(handle);
  if (!entry) return;
  Record record = BuildOutput(*entry);
  EncodeRecord(record, out);
  ++*count;
}

// Emits the CELL record followed by its cell-children group (persistent then
// temporary), shared by interior and exterior cells.
void EditSession::EncodeCellChildren(GlobalFormId cell, base::Vector<u8>* out, u32* count) {
  EncodeEntry(cell, out, count);  // the CELL record

  const CellChildren& children = cell_children_[cell.packed()];
  u32 label = Ref(cell).value;
  base::Vector<u8> children_body;
  if (!children.persistent.empty()) {
    base::Vector<u8> body;
    for (GlobalFormId ref : children.persistent) EncodeEntry(ref, &body, count);
    EmitGroup(kGroupCellPersistent, label, ByteSpan(body.data(), body.size()), &children_body);
  }
  if (!children.temporary.empty()) {
    base::Vector<u8> body;
    for (GlobalFormId ref : children.temporary) EncodeEntry(ref, &body, count);
    EmitGroup(kGroupCellTemporary, label, ByteSpan(body.data(), body.size()), &children_body);
  }
  EmitGroup(kGroupCellChildren, label, ByteSpan(children_body.data(), children_body.size()), out);
}

base::Vector<u8> EditSession::BuildCellGroup(u32* count) {
  if (cell_order_.empty()) return {};
  // Bin cells by interior block / sub-block so the tree matches the games'
  // layout. The numbers come from the last two decimal digits of the cell's
  // local form id: block = FormID mod 10 (ones digit), sub-block =
  // (FormID / 10) mod 10 (tens digit). Each group's label field is just that
  // plain number. Confirmed against xEdit (TwbMainRecord.UpdateInteriorCellGroup)
  // and UESP's Mod File Format/CELL. This engine's own loader keys cells off the
  // children group labels, not these numbers; the values exist purely so the
  // output is valid for the real games and displays correctly in xEdit/CK.
  std::map<u32, std::map<u32, base::Vector<u64>>> bins;
  for (u64 packed : cell_order_) {
    Entry* entry = FindEntry(GlobalFormId{static_cast<u16>(packed >> 32), static_cast<u32>(packed)});
    u32 local = entry->handle.local_id & 0xffffff;
    bins[local % 10][(local / 10) % 10].push_back(packed);
  }

  base::Vector<u8> top_body;
  for (const auto& [block, subblocks] : bins) {
    base::Vector<u8> block_body;
    for (const auto& [subblock, cells] : subblocks) {
      base::Vector<u8> sub_body;
      for (u64 packed : cells) {
        EncodeCellChildren(GlobalFormId{static_cast<u16>(packed >> 32), static_cast<u32>(packed)},
                           &sub_body, count);
      }
      EmitGroup(kGroupInteriorSubBlock, subblock, ByteSpan(sub_body.data(), sub_body.size()),
                &block_body);
    }
    EmitGroup(kGroupInteriorBlock, block, ByteSpan(block_body.data(), block_body.size()),
              &top_body);
  }
  base::Vector<u8> top;
  EmitGroup(kGroupTop, kCell, ByteSpan(top_body.data(), top_body.size()), &top);
  return top;
}

base::Vector<u8> EditSession::BuildWorldGroup(u32* count) {
  if (world_order_.empty()) return {};
  base::Vector<u8> top_body;
  for (u64 wpacked : world_order_) {
    GlobalFormId world{static_cast<u16>(wpacked >> 32), static_cast<u32>(wpacked)};
    EncodeEntry(world, &top_body, count);  // the WRLD record

    // Bin the worldspace's cells by exterior block / sub-block from their XCLC
    // grid coordinates. Per cell axis: sub-block coord = floor(grid / 8),
    // block coord = floor(grid / 32) (== floor(floor(grid/8) / 4), how xEdit
    // derives it). Floor division so negative grids round toward -inf. The
    // block group's label carries the block coords, the sub-block group's the
    // sub-block coords, each packed X-high/Y-low by PackGrid. Confirmed against
    // xEdit (wbSubBlockFromGridCell / wbBlockFromSubBlock / wbGridCellToGroupLabel)
    // and UESP's Mod File Format/CELL.
    std::map<std::pair<i32, i32>, std::map<std::pair<i32, i32>, base::Vector<u64>>> bins;
    for (GlobalFormId cell : world_cells_[wpacked]) {
      Entry* entry = FindEntry(cell);
      i32 x = 0, y = 0;
      if (entry) {
        for (const Subrecord& sub : entry->record.subrecords) {
          if (sub.type == kXclc && sub.data.size() >= 8) {
            std::memcpy(&x, sub.data.data(), 4);
            std::memcpy(&y, sub.data.data() + 4, 4);
            break;
          }
        }
      }
      bins[{FloorDiv(x, 32), FloorDiv(y, 32)}][{FloorDiv(x, 8), FloorDiv(y, 8)}].push_back(
          cell.packed());
    }

    base::Vector<u8> world_children;
    for (const auto& [block, subblocks] : bins) {
      base::Vector<u8> block_body;
      for (const auto& [subblock, cells] : subblocks) {
        base::Vector<u8> sub_body;
        for (u64 packed : cells) {
          EncodeCellChildren(
              GlobalFormId{static_cast<u16>(packed >> 32), static_cast<u32>(packed)}, &sub_body,
              count);
        }
        EmitGroup(kGroupExteriorSubBlock, PackGrid(subblock.first, subblock.second),
                  ByteSpan(sub_body.data(), sub_body.size()), &block_body);
      }
      EmitGroup(kGroupExteriorBlock, PackGrid(block.first, block.second),
                ByteSpan(block_body.data(), block_body.size()), &world_children);
    }
    EmitGroup(kGroupWorldChildren, Ref(world).value,
              ByteSpan(world_children.data(), world_children.size()), &top_body);
  }
  base::Vector<u8> top;
  EmitGroup(kGroupTop, kWrld, ByteSpan(top_body.data(), top_body.size()), &top);
  return top;
}

base::Vector<u8> EditSession::BuildDialGroup(u32* count) {
  if (dial_order_.empty()) return {};
  base::Vector<u8> top_body;
  for (u64 packed : dial_order_) {
    GlobalFormId dial{static_cast<u16>(packed >> 32), static_cast<u32>(packed)};
    EncodeEntry(dial, &top_body, count);  // the DIAL record

    base::Vector<u8> infos_body;
    for (GlobalFormId info : topic_infos_[packed]) EncodeEntry(info, &infos_body, count);
    EmitGroup(kGroupTopicChildren, Ref(dial).value, ByteSpan(infos_body.data(), infos_body.size()),
              &top_body);
  }
  base::Vector<u8> top;
  EmitGroup(kGroupTop, kDial, ByteSpan(top_body.data(), top_body.size()), &top);
  return top;
}

bool EditSession::WriteStringFiles(const std::string& plugin_path) {
  namespace fs = std::filesystem;
  fs::path plugin(plugin_path);
  std::string base = plugin.stem().string();  // plugin name without extension
  fs::path dir = plugin.parent_path() / "strings";
  std::error_code ec;
  fs::create_directories(dir, ec);

  const std::string& lang = profile_.string_language;
  const std::string prefix = (dir / (base + "_" + lang)).string();
  bool ok = true;
  // Each file is written only when it holds strings; the id space is shared.
  if (strings_.size()) ok &= strings_.Save(prefix + ".strings", /*length_prefixed=*/false);
  if (dlstrings_.size()) ok &= dlstrings_.Save(prefix + ".dlstrings", /*length_prefixed=*/true);
  if (ilstrings_.size()) ok &= ilstrings_.Save(prefix + ".ilstrings", /*length_prefixed=*/true);
  return ok;
}

bool EditSession::Save(const std::string& path, const SaveOptions& options) {
  const bool localized = options.localized || localized_;
  PluginWriter writer(profile_);
  writer.set_author(options.author)
      .set_description(options.description)
      .set_master(options.is_master)
      .set_light(options.is_light)
      .set_localized(localized);
  for (const std::string& master : masters_) writer.add_master(master);
  writer.set_next_object_id(next_local_id_);

  // Flat pass: every entry not claimed by a nested structure.
  for (u64 packed : order_of_entries_) {
    if (claimed_.count(packed)) continue;
    Record out = BuildOutput(entries_[packed]);
    writer.AddRecord(out);
  }

  // Nested passes: CELL and DIAL subtrees.
  u32 cell_count = 0;
  base::Vector<u8> cell_group = BuildCellGroup(&cell_count);
  if (!cell_group.empty()) writer.AddPrebuiltGroup(cell_group, cell_count);

  u32 dial_count = 0;
  base::Vector<u8> dial_group = BuildDialGroup(&dial_count);
  if (!dial_group.empty()) writer.AddPrebuiltGroup(dial_group, dial_count);

  u32 world_count = 0;
  base::Vector<u8> world_group = BuildWorldGroup(&world_count);
  if (!world_group.empty()) writer.AddPrebuiltGroup(world_group, world_count);

  if (!writer.Save(path)) return false;
  if (localized) return WriteStringFiles(path);
  return true;
}

}  // namespace rec::bethesda
