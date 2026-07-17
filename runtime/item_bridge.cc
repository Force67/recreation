#include "item_bridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "actor_system.h"
#include "asset/asset_id.h"
#include "asset/mesh.h"
#include "bethesda/record.h"
#include "core/log.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "inventory/inventory.h"
#include "inventory/serialize.h"
#include "scene/components.h"
#include "world/cell_streaming.h"
#include "world/components.h"
#include "world/quest_world.h"

namespace rx {
namespace {

// Bethesda object space -> engine metres, the same constant the cell streamer
// applies to a placed ref's mesh (1 unit = 1.428 cm). A dropped item's entity
// carries this on its Transform.scale so its NIF renders at world size, and its
// collision box's shape-unit -> metre scale matches.
constexpr f32 kUnitsToMeters = 0.01428f;

// A stable, handle-independent identity for the player's inventory entity, so
// rx SaveInventories/LoadInventories (keyed by scene::Guid) reattaches the saved
// stacks to the freshly spawned player each session. 0x14 is Skyrim's player ref
// form id; reused here purely as a convenient constant.
constexpr u64 kPlayerInventoryGuid = 0x7265637069746c76ull;  // "recpitlv"

// Base record types the player can pick up and carry. WEAP/MISC/BOOK/INGR/ALCH/
// AMMO/KEYM/SLGM carry a world model the drop path renders; ARMO is accepted for
// carrying but has no static world model (its ground visual is a biped model the
// engine does not build yet), so a dropped ARMO falls back to an invisible box.
bool IsItemType(u32 type) {
  switch (type) {
    case FourCc('W', 'E', 'A', 'P'):
    case FourCc('A', 'R', 'M', 'O'):
    case FourCc('M', 'I', 'S', 'C'):
    case FourCc('B', 'O', 'O', 'K'):
    case FourCc('I', 'N', 'G', 'R'):
    case FourCc('A', 'L', 'C', 'H'):
    case FourCc('A', 'M', 'M', 'O'):
    case FourCc('K', 'E', 'Y', 'M'):
    case FourCc('S', 'L', 'G', 'M'):
      return true;
    default:
      return false;
  }
}

// { u32 value; f32 weight } DATA layout shared by the common carry items.
bool IsValueWeightType(u32 type) {
  switch (type) {
    case FourCc('W', 'E', 'A', 'P'):
    case FourCc('A', 'R', 'M', 'O'):
    case FourCc('M', 'I', 'S', 'C'):
    case FourCc('I', 'N', 'G', 'R'):
    case FourCc('S', 'L', 'G', 'M'):
    case FourCc('K', 'E', 'Y', 'M'):
      return true;
    default:
      return false;
  }
}

// --- little-endian byte helpers for the per-profile item blob ---
void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutU16(std::vector<u8>& b, u16 v) {
  b.push_back(u8(v));
  b.push_back(u8(v >> 8));
}
void PutU64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (8 * i)));
}
void PutBytes(std::vector<u8>& b, const std::vector<u8>& v) {
  PutU32(b, u32(v.size()));
  b.insert(b.end(), v.begin(), v.end());
}

struct Reader {
  const u8* p;
  const u8* end;
  bool ok = true;
  Reader(const u8* d, size_t n) : p(d), end(d + n) {}
  u32 U32() {
    if (p + 4 > end) { ok = false; return 0; }
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(p[i]) << (8 * i);
    p += 4;
    return v;
  }
  u16 U16() {
    if (p + 2 > end) { ok = false; return 0; }
    u16 v = u16(p[0]) | u16(p[1]) << 8;
    p += 2;
    return v;
  }
  u64 U64() {
    if (p + 8 > end) { ok = false; return 0; }
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64(p[i]) << (8 * i);
    p += 8;
    return v;
  }
  std::vector<u8> Bytes() {
    u32 n = U32();
    if (p + n > end) { ok = false; return {}; }
    std::vector<u8> v(p, p + n);
    p += n;
    return v;
  }
};

std::filesystem::path ConfigDir() {
  namespace fs = std::filesystem;
  if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x) / "recreation";
  if (const char* h = std::getenv("HOME")) return fs::path(h) / ".config" / "recreation";
  return fs::path("recreation_config");
}

std::string SanitizeProfile(const std::string& name) {
  std::string out;
  for (char c : name) {
    out.push_back((std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_');
  }
  return out.empty() ? "player" : out;
}

}  // namespace

// "RITM" v1: def table + removed-ref set + rx inventory blob + rx world-item blob.
static constexpr u32 kBlobMagic = 0x4d544952u;  // 'RITM'
static constexpr u32 kBlobVersion = 1;

ItemBridge::ItemBridge(EngineContext& ctx, ActorSystem* actors) : ctx_(ctx), actors_(actors) {
  world_store_.set_cell_size(32.0f);  // metres; a coarse spatial bucket for dormant loot
}

std::string ItemBridge::SavePath() const {
  std::string profile = SanitizeProfile(ctx_.config ? ctx_.config->player_name : "player");
  return (ConfigDir() / "profiles" / profile / "items.bin").string();
}

bool ItemBridge::IsItemBase(bethesda::GlobalFormId base) const {
  if (!ctx_.records) return false;
  const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(base);
  return stored && IsItemType(stored->header.type);
}

ecs::Entity ItemBridge::PlayerInventoryEntity() {
  ecs::Entity player = actors_->PlayerEntity();
  if (!ctx_.world->IsAlive(player)) return ecs::kInvalidEntity;
  if (!ctx_.world->Has<inventory::Inventory>(player)) {
    ctx_.world->Add(player, inventory::Inventory{});
    // A stable Guid so the saved inventory reattaches to next session's player.
    if (!ctx_.world->Has<scene::Guid>(player))
      ctx_.world->Add(player, scene::Guid{kPlayerInventoryGuid});
  }
  return player;
}

bool ItemBridge::BuildDef(bethesda::GlobalFormId base, inventory::ItemDef* out) {
  if (!ctx_.records) return false;
  const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(base);
  if (!stored || !IsItemType(stored->header.type)) return false;
  bethesda::Record rec;
  if (!ctx_.records->Parse(base, &rec)) return false;
  const u32 type = rec.header.type;

  inventory::ItemDef def;
  def.name_hash = base.packed();
  def.max_stack = 1000;  // clutter stacks freely; unique instances use distinct payloads
  def.flags = type;      // game-defined: the base record signature

  // Weight / value from DATA (see skyrim_bindings_item_data.cc for the layout).
  f32 weight = 0.0f;
  u32 value = 0;
  if (const bethesda::Subrecord* data = rec.Find(FourCc('D', 'A', 'T', 'A'))) {
    if (IsValueWeightType(type) && data->data.size() >= 8) {
      std::memcpy(&value, data->data.data(), 4);
      std::memcpy(&weight, data->data.data() + 4, 4);
    } else if (type == FourCc('A', 'L', 'C', 'H') && data->data.size() >= 4) {
      std::memcpy(&weight, data->data.data(), 4);  // ALCH keeps weight alone in DATA
    }
  }
  def.weight = weight > 0.0f ? weight : 0.0f;
  def.payload = value;  // gold value, game-defined per-definition data

  // Mass: derive a plausible kg from the game weight, clamped so a feather and a
  // greatsword both tumble believably.
  def.mass = std::clamp(weight > 0.0f ? weight * 0.5f : 0.2f, 0.1f, 100.0f);
  def.friction = 0.6f;
  def.restitution = 0.05f;
  def.scale = kUnitsToMeters;  // the box's shape-unit -> metre scale (mesh is game-unit space)

  // World mesh + a box collision shape from its bounds, via the streamer's model
  // pipeline (converts + uploads to the shared renderer, salted for this domain).
  asset::AssetId render_id{};
  const asset::Mesh* mesh = ctx_.streamer ? ctx_.streamer->PrepareItemModel(base, &render_id)
                                          : nullptr;
  if (mesh) {
    def.world_mesh = render_id;
    // Collision box from the lod-0 vertex AABB. Item NIFs are NOT symmetric about
    // the origin (a weapon models the grip near the origin with the blade running
    // far out one axis), so a box sized/centred on max|v| is ~2x too big and sits
    // off-centre: dropped, it rests on the floor with the visual mesh floating at
    // the oversized box's centre. Size the box to the true extent (max-min)/2 and
    // offset it to the AABB centre via a placed child so it hugs the mesh and the
    // dropped item lies flat on the ground.
    f32 mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    if (!mesh->lods.empty()) {
      for (const asset::Vertex& v : mesh->lods[0].vertices) {
        for (int k = 0; k < 3; ++k) {
          mn[k] = std::min(mn[k], v.position[k]);
          mx[k] = std::max(mx[k], v.position[k]);
        }
      }
    }
    if (mn[0] > mx[0]) { mn[0] = mn[1] = mn[2] = -2.0f; mx[0] = mx[1] = mx[2] = 2.0f; }
    // Half extents and centre offset, in game units (scaled by def.scale later).
    const f32 half[3] = {std::max((mx[0] - mn[0]) * 0.5f, 2.0f),
                         std::max((mx[1] - mn[1]) * 0.5f, 2.0f),
                         std::max((mx[2] - mn[2]) * 0.5f, 2.0f)};
    const f32 ctr[3] = {(mx[0] + mn[0]) * 0.5f, (mx[1] + mn[1]) * 0.5f, (mx[2] + mn[2]) * 0.5f};
    physics::ShapeDesc box;
    box.kind = physics::ShapeDesc::Kind::kBox;
    box.half_extents = Vec3{half[0], half[1], half[2]};
    def.shape.kind = physics::ShapeDesc::Kind::kPlaced;
    def.shape.transform[0] = def.shape.transform[5] = def.shape.transform[10] =
        def.shape.transform[15] = 1.0f;  // identity basis
    def.shape.transform[12] = ctr[0];
    def.shape.transform[13] = ctr[1];
    def.shape.transform[14] = ctr[2];
    def.shape.children = {box};
  } else {
    // No world model (e.g. ARMO): an invisible ~10 cm box still drops + persists.
    def.shape.kind = physics::ShapeDesc::Kind::kBox;
    def.shape.half_extents = Vec3{7.0f, 7.0f, 7.0f};
  }
  *out = def;
  return true;
}

inventory::ItemDefId ItemBridge::DefForBase(bethesda::GlobalFormId base) {
  const u64 key = base.packed();
  if (auto it = base_to_def_.find(key); it != base_to_def_.end()) return it->second;
  inventory::ItemDef def;
  if (!BuildDef(base, &def)) return inventory::kInvalidItemDef;
  const inventory::ItemDefId id = next_def_id_++;
  catalog_.Register(id, def);
  base_to_def_[key] = id;
  def_to_base_[id] = base;
  return id;
}

bool ItemBridge::TryPickUp(u64 ref_handle) {
  if (!ctx_.records || !ctx_.world) return false;
  if (removed_refs_.count(ref_handle)) return false;  // already taken (idempotent)
  const bethesda::GlobalFormId refr{static_cast<u16>(ref_handle >> 32),
                                    static_cast<u32>(ref_handle & 0xffffffffu)};
  const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(refr);
  if (!stored) return false;
  bethesda::Record record;
  if (!ctx_.records->Parse(refr, &record)) return false;
  const bethesda::Subrecord* name = record.Find(FourCc('N', 'A', 'M', 'E'));
  if (!name || name->data.size() < 4) return false;
  u32 base_raw;
  std::memcpy(&base_raw, name->data.data(), 4);
  const bethesda::GlobalFormId base =
      ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw}, stored->winning_plugin);
  if (!IsItemBase(base)) return false;  // not an item: let other affordances handle it

  const inventory::ItemDefId def = DefForBase(base);
  if (def == inventory::kInvalidItemDef) return false;

  const ecs::Entity player = PlayerInventoryEntity();
  if (!ctx_.world->IsAlive(player)) return false;
  inventory::Inventory* inv = ctx_.world->Get<inventory::Inventory>(player);
  if (!inv) return false;

  // Placed stack count (XCNT), else a single unit.
  u32 count = 1;
  if (const bethesda::Subrecord* xcnt = record.Find(FourCc('X', 'C', 'N', 'T'));
      xcnt && xcnt->data.size() >= 4) {
    i32 c;
    std::memcpy(&c, xcnt->data.data(), 4);
    if (c > 0) count = static_cast<u32>(c);
  }
  const u32 added = inventory::AddItem(*inv, catalog_, def, count);
  if (added == 0) return false;

  // Remove the world reference: find its ECS entity by form link and destroy it,
  // unregister it from the quest world, and remember it so the streamer never
  // re-places it (persistent removal across streaming + reload).
  ecs::Entity ref_entity = ecs::kInvalidEntity;
  ctx_.world->Each<world::FormLink>([&](ecs::Entity e, world::FormLink& link) {
    if (link.form.packed() == ref_handle) ref_entity = e;
  });
  if (ctx_.world->IsAlive(ref_entity)) ctx_.world->Destroy(ref_entity);
  if (ctx_.quest_world) ctx_.quest_world->Unregister(ref_handle);
  removed_refs_.insert(ref_handle);

  std::string display = RecordNameFor(base);
  if (display.empty()) display = "item";
  if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("Picked up " + display);
  RX_INFO("item: picked up '{}' x{} (ref 0x{:x}, base {:04x}:{:06x})", display, added, ref_handle,
          base.plugin, base.local_id);
  Save();  // pickups are cheap and rare relative to frames; persist immediately
  return true;
}

std::string ItemBridge::RecordNameFor(bethesda::GlobalFormId id) const {
  if (!ctx_.records) return {};
  bethesda::Record record;
  if (!ctx_.records->Parse(id, &record)) return {};
  const bethesda::Subrecord* full = record.Find(FourCc('F', 'U', 'L', 'L'));
  if (!full) return {};
  if (ctx_.strings && full->data.size() >= 4) {
    u32 string_id;
    std::memcpy(&string_id, full->data.data(), 4);
    if (const base::String* s = ctx_.strings->Find(string_id)) return std::string(s->c_str());
  }
  return record.GetString(FourCc('F', 'U', 'L', 'L'));
}

void ItemBridge::DropLast() {
  if (!ctx_.world || !ctx_.physics) return;
  const ecs::Entity player = PlayerInventoryEntity();
  if (!ctx_.world->IsAlive(player)) return;
  inventory::Inventory* inv = ctx_.world->Get<inventory::Inventory>(player);
  if (!inv) return;

  // The most recently added stack: the last non-empty entry.
  int entry = -1;
  for (int i = static_cast<int>(inv->entries.size()) - 1; i >= 0; --i) {
    if (inv->entries[i].count > 0) { entry = i; break; }
  }
  if (entry < 0) {
    if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("Nothing to drop");
    return;
  }

  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;
  // Horizontal facing from the walk camera; drop in front at chest height.
  Vec3 fwd = ctx_.walk_target - ctx_.walk_eye;
  fwd.y = 0.0f;
  const f32 len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
  fwd = len > 1e-4f ? Vec3{fwd.x / len, 0.0f, fwd.z / len} : Vec3{0, 0, 1};

  scene::Transform spawn;
  spawn.position[0] = ppos.x + fwd.x * 0.6f;
  spawn.position[1] = ppos.y + 1.0f;
  spawn.position[2] = ppos.z + fwd.z * 0.6f;
  spawn.rotation[0] = 0; spawn.rotation[1] = 0; spawn.rotation[2] = 0; spawn.rotation[3] = 1;
  spawn.scale = kUnitsToMeters;  // render the NIF at world size (matches placed refs)

  const inventory::ItemDefId item = inv->entries[entry].item;
  const inventory::ItemDef* def = catalog_.Find(item);
  const f32 mass = def ? def->mass : 1.0f;
  // A gentle forward toss plus a little lift so it arcs and tumbles to rest.
  const Vec3 impulse{fwd.x * mass * 2.5f, mass * 1.2f, fwd.z * mass * 2.5f};

  const ecs::Entity dropped =
      inventory::DropItem(*ctx_.world, *ctx_.physics, catalog_, player, entry, 1, spawn, impulse);
  if (dropped == ecs::kInvalidEntity) return;

  std::string display = def ? RecordNameFor(def_to_base_.count(item) ? def_to_base_.at(item)
                                                                     : bethesda::GlobalFormId{})
                            : std::string();
  if (display.empty()) display = "item";
  if (ctx_.game_ui) ctx_.game_ui->FlashQuestUpdate("Dropped " + display);
  RX_INFO("item: dropped '{}' at ({:.2f},{:.2f},{:.2f})", display, spawn.position[0],
          spawn.position[1], spawn.position[2]);
  Save();
}

void ItemBridge::Update(f32 dt) {
  if (!ctx_.world || !ctx_.physics) return;
  if (!loaded_ && actors_->HasPlayer()) OnPlayerReady();
  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;

  // Mirror awake body transforms into ECS transforms, then hibernate settled
  // loot beyond the far radius into the store and wake stored loot back near the
  // player. Radii: hibernate comfortably past the streaming bubble (load_radius 3
  // cells ~ 175 m), wake smaller for hysteresis so a boundary item never thrashes.
  inventory::SyncWorldItems(*ctx_.world, *ctx_.physics);
  constexpr f32 kHibernateRadius = 256.0f;
  constexpr f32 kWakeRadius = 192.0f;
  inventory::HibernateDistantWorldItems(*ctx_.world, *ctx_.physics, world_store_, ppos,
                                        kHibernateRadius);
  inventory::WakeWorldItemsNear(*ctx_.world, *ctx_.physics, catalog_, world_store_, ppos,
                                kWakeRadius);

  // Cheap periodic autosave (blobs are tiny); pickups/drops also save on the spot.
  autosave_timer_ += dt;
  if (autosave_timer_ >= 120.0f) {
    autosave_timer_ = 0.0f;
    Save();
  }

  MaybeRunProbe(dt);
}

void ItemBridge::MaybeRunProbe(f32 dt) {
  static const bool kProbe = std::getenv("RX_ITEM_PROBE") != nullptr;
  if (!kProbe || probe_done_) return;
  probe_timer_ += dt;
  if (probe_timer_ < 6.0f) return;  // let cells stream in first
  probe_done_ = true;

  Vec3 ppos;
  if (!actors_->PlayerWorldPos(&ppos)) return;
  // Find the nearest loose item reference to the player.
  u64 best = 0;
  f32 best_d2 = 1e30f;
  ctx_.world->Each<world::FormLink, world::Transform>(
      [&](ecs::Entity, world::FormLink& link, world::Transform& t) {
        const u64 handle = link.form.packed();
        const bethesda::GlobalFormId refr{static_cast<u16>(handle >> 32),
                                          static_cast<u32>(handle & 0xffffffffu)};
        const bethesda::RecordStore::StoredRecord* stored = ctx_.records->Find(refr);
        if (!stored) return;
        bethesda::Record rec;
        if (!ctx_.records->Parse(refr, &rec)) return;
        const bethesda::Subrecord* name = rec.Find(FourCc('N', 'A', 'M', 'E'));
        if (!name || name->data.size() < 4) return;
        u32 base_raw;
        std::memcpy(&base_raw, name->data.data(), 4);
        const bethesda::GlobalFormId base =
            ctx_.records->ResolveFrom(bethesda::RawFormId{base_raw}, stored->winning_plugin);
        if (!IsItemBase(base)) return;
        const f32 dx = t.position[0] - ppos.x, dy = t.position[1] - ppos.y,
                  dz = t.position[2] - ppos.z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) { best_d2 = d2; best = handle; }
      });

  if (best == 0) {
    RX_INFO("ITEM_PROBE: no loose item reference found in the loaded world");
    return;
  }
  RX_INFO("ITEM_PROBE: nearest item ref 0x{:x} at {:.1f} m; picking up", best, std::sqrt(best_d2));
  const bool picked = TryPickUp(best);
  RX_INFO("ITEM_PROBE: pickup {} -> inventory has {} stack(s)", picked ? "OK" : "FAILED",
          [&] {
            const ecs::Entity p = actors_->PlayerEntity();
            const inventory::Inventory* inv = ctx_.world->Get<inventory::Inventory>(p);
            u32 n = 0;
            if (inv)
              for (const auto& e : inv->entries)
                if (e.count > 0) ++n;
            return n;
          }());
  DropLast();
  RX_INFO("ITEM_PROBE: dropped; removed_refs={}, world-item entities follow at next sync", removed_refs_.size());
}

void ItemBridge::OnPlayerReady() {
  PlayerInventoryEntity();  // ensure the player carries an Inventory + stable Guid
  // The cell streamer skips any ref we have marked removed, so a picked-up item
  // never re-places when its cell streams back in.
  if (ctx_.streamer)
    ctx_.streamer->set_ref_suppressor([this](u64 handle) { return removed_refs_.count(handle) != 0; });
  if (!loaded_) {
    Load();
    loaded_ = true;
    // Cells around the spawn may have streamed in before the suppressor was set;
    // destroy any already-placed ref we know was removed.
    if (!removed_refs_.empty() && ctx_.world) {
      std::vector<ecs::Entity> victims;
      ctx_.world->Each<world::FormLink>([&](ecs::Entity e, world::FormLink& link) {
        if (removed_refs_.count(link.form.packed())) victims.push_back(e);
      });
      for (ecs::Entity e : victims) {
        if (ctx_.quest_world) ctx_.quest_world->Unregister(ctx_.world->Get<world::FormLink>(e)->form.packed());
        ctx_.world->Destroy(e);
      }
    }
  }
}

void ItemBridge::Save() const {
  if (!ctx_.world) return;
  std::vector<u8> blob;
  PutU32(blob, kBlobMagic);
  PutU32(blob, kBlobVersion);

  // Def table: id -> base form, so saved inventory/world-item def ids rebind to
  // the same record next session.
  PutU32(blob, u32(def_to_base_.size()));
  for (const auto& [id, base] : def_to_base_) {
    PutU32(blob, id);
    PutU16(blob, base.plugin);
    PutU32(blob, base.local_id);
  }

  // Removed refs.
  PutU32(blob, u32(removed_refs_.size()));
  for (u64 h : removed_refs_) PutU64(blob, h);

  // rx inventory + world-item blobs.
  PutBytes(blob, inventory::SaveInventories(*ctx_.world));
  PutBytes(blob, inventory::SaveWorldItems(*ctx_.world, world_store_));

  const std::string path = SavePath();
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    RX_WARN("item: could not write {}", path);
    return;
  }
  out.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
  RX_INFO("item: saved {} bytes to {}", blob.size(), path);
}

bool ItemBridge::Load() {
  const std::string path = SavePath();
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;  // fresh profile
  std::vector<u8> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Reader r(blob.data(), blob.size());
  if (r.U32() != kBlobMagic || r.U32() != kBlobVersion) {
    RX_WARN("item: {} is not a v{} item blob, ignoring", path, kBlobVersion);
    return false;
  }

  // Def table: rebuild + register each def under its saved id so the inventory
  // and world items below resolve.
  const u32 defs = r.U32();
  for (u32 i = 0; i < defs && r.ok; ++i) {
    const inventory::ItemDefId id = r.U32();
    const u16 plugin = r.U16();
    const u32 local = r.U32();
    const bethesda::GlobalFormId base{plugin, local};
    inventory::ItemDef def;
    if (!BuildDef(base, &def)) continue;  // record gone (mod removed): drop the def
    catalog_.Register(id, def);
    base_to_def_[base.packed()] = id;
    def_to_base_[id] = base;
    next_def_id_ = std::max(next_def_id_, id + 1);
  }

  const u32 removed = r.U32();
  for (u32 i = 0; i < removed && r.ok; ++i) removed_refs_.insert(r.U64());

  const std::vector<u8> inv_blob = r.Bytes();
  const std::vector<u8> wi_blob = r.Bytes();
  if (!r.ok) {
    RX_WARN("item: {} truncated, partial load", path);
    return false;
  }
  if (!inv_blob.empty()) inventory::LoadInventories(*ctx_.world, inv_blob);
  if (!wi_blob.empty() && ctx_.physics)
    inventory::LoadWorldItems(*ctx_.world, *ctx_.physics, catalog_, world_store_, wi_blob);

  // Count what came back, for restart verification.
  u32 live_items = 0;
  ctx_.world->Each<inventory::WorldItem>([&](ecs::Entity, inventory::WorldItem&) { ++live_items; });
  u32 inv_stacks = 0;
  const ecs::Entity player = actors_->PlayerEntity();
  if (const inventory::Inventory* inv = ctx_.world->Get<inventory::Inventory>(player))
    for (const auto& e : inv->entries)
      if (e.count > 0) ++inv_stacks;
  RX_INFO("item: loaded {} defs, {} removed refs, {} inventory stack(s), {} live + {} dormant world "
          "item(s) from {}",
          defs, removed, inv_stacks, live_items, world_store_.size(), path);
  return true;
}

}  // namespace rx
