# Bethesda plugin write architecture — scoping

Goal: make the `engine/bethesda/` content layer **symmetric** — anything we can load,
we can also write. Two concrete products, sharing one core:

1. **Authoring** — create a new `.esp`/`.esm`/`.esl` that masters the base game and
   contains new records plus overrides of existing forms.
2. **Modifying** — open an existing plugin, mutate/add/delete records, and save it back
   (faithfully, preserving records and fields we don't understand).

This document scopes the architecture. No code yet.

---

## 1. What the read side gives us (and doesn't)

All plugin handling lives in `engine/bethesda/`. Relevant facts for a writer:

- **Generic, schema-less container.** `record.h`: `RecordHeader` (24 B, `static_assert`ed),
  `GroupHeader` (24 B), `Subrecord{u32 type; ByteSpan data}`, `Record{header; subrecords}`.
  There is **no typed per-record model** — no `Weapon`/`Cell`/`Npc` struct. `Subrecord::data`
  is `std::span<const u8>` pointing **zero-copy into the owning `PluginFile` bytes**.
- **Field readers are dispersed and lossy.** ~159 `FourCc(...)` sites across ~40+ record
  types read fields by hardcoded offset (`memcpy(&x, sub->data.data()+8, 2)`) or by an ordered
  subrecord state machine (QUST/PACK/SCEN/DIAL). They extract only what the engine needs and
  **discard the rest**. So they cannot be reversed into a writer.
- **`RecordStore` is a lazy merged index** (`load_order.h`): packed `GlobalFormId` →
  `StoredRecord{header; ByteSpan payload; u16 winning_plugin}`, last-loaded-wins, plus derived
  spatial/dialogue indices. Payload stays raw (possibly compressed); `Parse()` splits on demand.
- **GRUPs are flattened on read.** `VisitRecordsRaw` walks linearly and tracks a transient
  `GroupContext{worldspace, cell, cell_group_type, dialogue}` from group labels — but this
  provenance is **not persisted per record**.
- **Compression:** `kRecordFlagCompressed` (0x00040000); payload = `u32 uncompressed_size`
  then zlib stream. `ZlibInflate` exists; **there is no deflate**.
- **No write path exists.** Everything opens via `ifstream`. Greenfield.

**Consequence:** the only faithful thing to serialize from is the generic
`Record`/`Subrecord` container, which preserves every subrecord's bytes and order. The typed
offset-readers are out of the picture for writing.

---

## 2. Core design

### 2.1 Editable, owning mirror + copy-on-write delta

The read model is zero-copy spans into read-only file bytes; we cannot mutate those. Introduce
an **owning** editable form, and apply edits as a **delta over the existing `RecordStore`**:

```
struct EditableSubrecord { u32 type; base::Vector<u8> data; };      // owns bytes
struct EditableRecord {
  RecordHeader header;                       // type, flags, form_id, form_version
  base::Vector<EditableSubrecord> fields;    // order preserved; unknown fields kept
};
```

```
class EditSession {                          // the write-side working set
 public:
  explicit EditSession(const RecordStore* base);   // base may be null for pure authoring

  EditableRecord* Edit(GlobalFormId id);     // COW: materialize from base's raw payload
  EditableRecord* Create(u32 type);          // allocates a new local FormID (see 2.4)
  void            Delete(GlobalFormId id);    // tombstone (ITM/deleted flag or drop)

  // ... serialize (see 2.3)
 private:
  const RecordStore* base_;
  base::UnorderedMap<u64, EditableRecord> overrides_;   // packed id -> edited record
  base::UnorderedMap<u64, ...>           created_;
  base::UnorderedSet<u64>                deleted_;
};
```

- **`Edit(id)`** decompresses+splits the base record once into an owning `EditableRecord`
  (round-trippable: unknown subrecords survive because we keep *all* of them).
- **Untouched records serialize by memcpy-through** from their original raw span — so
  saving a modified base plugin doesn't re-encode millions of records, and the
  "load the base game in ~2 s" property is preserved. We only pay for what we edit.
- Field-level ergonomics (a `WeaponView` that knows `DATA` layout) are an **optional typed
  overlay layered later** (§6), mirroring exactly how the read side already works.

### 2.2 Writer core (serializer)

New `writer.{h,cc}`. Inverts `ParseSubrecords` / `ParseRecordPayload`:

- **Subrecord encode:** `[u32 type][u16 size][bytes]`. If `bytes.size() > 0xFFFF`, emit the
  `XXXX` escape first (`[XXXX][u16 4][u32 real_size]`) then the field with `size16 = 0`.
- **Record encode:** compute `data_size` over the encoded subrecords; if compressing, deflate
  and prefix the `u32` uncompressed size, and set `data_size` to the *compressed* length incl.
  the 4-byte prefix (matches the reader's `Take(data_size)` → `DecompressRecord`). Set the
  compressed flag accordingly.
- **Deleted records:** honor `kRecordFlagDeleted` (0x20) — the reader skips them; a delete can
  either drop the record or emit a header-only deleted stub (needed when overriding a base form
  to remove it — a bare drop wouldn't override the master).

### 2.3 Group tree reconstruction (the hard part)

Reading flattens GRUPs; writing must **rebuild the tree and back-patch every `group_size`**.
`group_size` counts the 24-byte GRUP header **plus** all contained records and nested groups —
an off-by-24 crashes the game.

- **Flat types** (WEAP/ARMO/SPEL/GLOB/…): bin by top-level type into one type-group each.
  Trivial; covers the bulk of authoring.
- **Nested types** (WRLD→block→subblock→CELL→persistent/temporary children→REFR/ACHR/LAND;
  DIAL→INFO children): need the nesting rebuilt. This requires **persisting group provenance
  during load** — the `GroupContext` the reader already computes but discards, plus the
  block/subblock grid math. This is scoped into P2, not P0/P1.

A `GroupTreeBuilder` takes the flat record set + provenance and emits headers with correct
back-patched sizes, in canonical group order.

### 2.4 FormIDs & master table (inverse of `LoadOrder::Resolve`)

On disk a `RawFormId`'s top byte indexes the plugin's **own** master list. The writer needs the
inverse of `LoadOrder::Resolve`:

```
RawFormId Encode(GlobalFormId id, MasterTable& out_masters);   // adds masters on demand
```

- A reference to a form defined by plugin P becomes a `RawFormId` whose mod-index = P's slot in
  **this output plugin's** master list; if P isn't yet a master, append it (order defines index).
- **New records** get a local id in the output plugin's own space (mod-index = number of masters,
  i.e. "self"; or the `0xFE` ESL slot). Allocation tracks a next-object-id high-water mark.
- **ESL** constrains the local id range (classic Skyrim `0x800–0xFFF`); enforce on allocation.

### 2.5 TES4 header bookkeeping

Emit a correct `TES4`: `HEDR{f32 version, u32 num_records, u32 next_object_id}`, `CNAM` (author),
`SNAM` (desc), one `MAST`(+`DATA` u64) per master **in order**, flags (master 0x1 / localized 0x80
/ light 0x200), optional `ONAM` overridden-form list (master files only, P3), `INTV`. `num_records`
= count of records emitted excluding `TES4`.

---

## 3. Proposed public API

```
// Authoring or modifying — both go through EditSession.
EditSession session(&record_store);          // or EditSession session(nullptr) for scratch

EditableRecord* w = session.Edit(weapon_id); // override an existing form
SetField(w, FourCc('F','U','L','L'), name_bytes);   // free helper, raw bytes

EditableRecord* n = session.Create(FourCc('W','E','A','P'));  // brand new form
session.Delete(unwanted_id);

PluginWriteOptions opt{ .out_name = "MyMod.esp", .light = false,
                        .author = "…", .compress = CompressPolicy::Never };
session.Save(data_dir + "/MyMod.esp", opt, profile);   // builds masters, groups, header
```

Raw-subrecord editing is the default (fidelity-first, minimal new code, unknown fields
preserved). Typed helpers are added case-by-case and, later, via the §6 schema overlay.

---

## 4. New files (all under `engine/bethesda/`)

| File | Contents |
|---|---|
| `record_edit.{h,cc}` | `EditableSubrecord`, `EditableRecord`, field helpers (`SetField`/`RemoveField`) |
| `edit_session.{h,cc}` | `EditSession`: COW `Edit`/`Create`/`Delete`, override/tombstone maps |
| `writer.{h,cc}` | subrecord/record encode, `XXXX` escape, `data_size`, compression flag, `Save` |
| `group_builder.{h,cc}` | rebuild GRUP tree, back-patch sizes, canonical ordering |
| `master_table.{h,cc}` | `Encode(GlobalFormId)→RawFormId`, on-demand master list, next-object-id |
| `compression.cc` (extend) | add `ZlibDeflate` to complement `ZlibInflate` |
| `load_order.{h,cc}` (extend) | persist per-record group provenance for P2 |

---

## 5. Phasing

- **P0 — Serializer core.** `EditableRecord`; encode subrecords (incl. `XXXX`), record header,
  `data_size`; flat TES4 + type-groups; uncompressed, non-localized. Round-trip test on simple
  record types. → gamemodes/tools can author WEAP/ARMO/SPEL/etc.
- **P1 — Delta + FormIDs + masters.** COW `EditSession`; override/delete/create; new-id
  allocation; `master_table` inverse-resolve; correct TES4 with masters. → valid mod `.esp`
  mastering the base game; can override existing forms.
- **P2 — Group-tree reconstruction.** Persist provenance; rebuild nested WRLD/CELL/DIAL groups
  with correct sizes. → worldspace/cell/dialogue authoring and editing.
- **P3 — Fidelity & completeness.** Byte-faithful full-file round-trip; `ZlibDeflate`
  compression; `ONAM`; ESL id-range enforcement; localized `.strings`/`.dlstrings`/`.ilstrings`
  writing.
- **P4 (optional, orthogonal) — declarative typed schema.** Field tables per (record, subrecord)
  driving *both* read and write; validated named-field edits; retires the ~159 ad-hoc read sites
  over time.

Both target products are reachable by end of P2: **authoring** lands at P1 (flat records) and
completes at P2 (cells/worlds); **modifying** works at P1 for flat records via memcpy-through of
untouched data, and P3 raises it to byte-faithful.

---

## 6. Optional typed schema overlay (P4)

A declarative table like `{WEAP: {DATA: [{damage, @8, u16}, …], DNAM: […]}}` would drive read and
write from one source of truth, giving named-field editing and validation. Large effort, and
strictly additive — the raw-subrecord model underneath stays the fidelity floor. Deferred; noted
so P0–P3 APIs don't foreclose it (helpers take `(record, fourcc, bytes)`, which a schema can wrap).

---

## 7. Risks / test strategy

- **Group-size accounting** (off-by-24) → hard crashes. Golden test: rebuild base plugin's group
  tree and diff sizes against the original.
- **Round-trip fidelity.** Load → `EditSession` → `Save` with no edits → re-parse must equal the
  original record set (P0/P1) and be byte-identical for a single file (P3). Test against real SE
  data (see the *Skyrim test data* memory; run with a `<data_dir>` arg, not in CI).
- **FormID/master encoding.** Verify `Encode` is a true inverse of `LoadOrder::Resolve` on a set
  of cross-plugin references.
- **Deflate parity.** The game's zlib must accept our stream; compare `ZlibInflate(ZlibDeflate(x))
  == x` and load output in-engine.
- **External validation.** Open produced plugins in xEdit / Creation Kit and in-engine to confirm
  they load and the game accepts them.
