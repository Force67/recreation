#include "gamenet/world_replication.h"

#include <cstring>

#include <nanobuf.h>

namespace rx::net {
namespace {

// One command is encoded as a fixed 40-byte little-endian record, carried as an
// opaque bytes entry in the outer message:
//   u8 op | u64 quest | u64 handle | u64 base | f32 x | f32 y | f32 z | u8 enabled
//   | u8 is_actor | u8 team
constexpr size_t kRecordSize = 1 + 8 + 8 + 8 + 4 + 4 + 4 + 1 + 1 + 1;

void AppendU8(std::vector<u8>& out, u8 v) { out.push_back(v); }
void AppendU32(std::vector<u8>& out, u32 v) {
  u8 buf[4];
  nanobuf::StoreLe<u32>(buf, v);
  out.insert(out.end(), buf, buf + 4);
}
void AppendU64(std::vector<u8>& out, u64 v) {
  u8 buf[8];
  nanobuf::StoreLe<u64>(buf, v);
  out.insert(out.end(), buf, buf + 8);
}
void AppendF32(std::vector<u8>& out, f32 v) {
  u32 bits;
  std::memcpy(&bits, &v, 4);
  AppendU32(out, bits);
}

std::vector<u8> EncodeRecord(const world::WorldCommand& c) {
  std::vector<u8> rec;
  rec.reserve(kRecordSize);
  AppendU8(rec, static_cast<u8>(c.op));
  AppendU64(rec, c.quest);
  AppendU64(rec, c.handle);
  AppendU64(rec, c.base);
  AppendF32(rec, c.pos[0]);
  AppendF32(rec, c.pos[1]);
  AppendF32(rec, c.pos[2]);
  AppendU8(rec, c.enabled ? 1 : 0);
  AppendU8(rec, c.is_actor ? 1 : 0);
  AppendU8(rec, static_cast<u8>(c.team));
  return rec;
}

bool DecodeRecord(const u8* data, size_t size, world::WorldCommand* out) {
  if (size != kRecordSize) return false;
  size_t pos = 0;
  auto u8at = [&] { return data[pos++]; };
  auto u32at = [&] {
    u32 v = nanobuf::LoadLe<u32>(data + pos);
    pos += 4;
    return v;
  };
  auto u64at = [&] {
    u64 v = nanobuf::LoadLe<u64>(data + pos);
    pos += 8;
    return v;
  };
  auto f32at = [&] {
    u32 bits = u32at();
    f32 v;
    std::memcpy(&v, &bits, 4);
    return v;
  };

  const u8 op = u8at();
  if (op > static_cast<u8>(world::WorldOp::kCleanupQuest)) return false;
  out->op = static_cast<world::WorldOp>(op);
  out->quest = u64at();
  out->handle = u64at();
  out->base = u64at();
  out->pos = {f32at(), f32at(), f32at()};
  out->enabled = u8at() != 0;
  out->is_actor = u8at() != 0;
  out->team = static_cast<i32>(u8at());
  out->has_mesh = false;  // clients resolve the mesh from `base` locally
  return true;
}

}  // namespace

std::vector<u8> EncodeWorldCommands(const std::vector<world::WorldCommand>& commands) {
  nanobuf::Writer writer;
  writer.Begin(/*fixed_len=*/6);  // 2-byte header + one 4-byte offset slot
  writer.PutOffsetList<world::WorldCommand>(
      /*slot=*/2, commands, [](nanobuf::Writer& w, const world::WorldCommand& c) {
        std::vector<u8> rec = EncodeRecord(c);
        return w.HeapBytes(rec);
      });
  return writer.TakeBuffer();
}

std::optional<std::vector<world::WorldCommand>> DecodeWorldCommands(ByteSpan data) {
  std::optional<nanobuf::View> view = nanobuf::View::Parse(data.data(), data.size());
  if (!view) return std::nullopt;
  std::optional<nanobuf::BytesList> records = view->BytesListAt(/*slot=*/2);
  if (!records) return std::nullopt;

  std::vector<world::WorldCommand> out;
  out.reserve(records->size());
  for (size_t i = 0; i < records->size(); ++i) {
    std::optional<nanobuf::BytesView> bytes = records->Get(i);
    if (!bytes) return std::nullopt;
    world::WorldCommand cmd;
    if (!DecodeRecord(bytes->data, bytes->size, &cmd)) return std::nullopt;
    out.push_back(cmd);
  }
  return out;
}

}  // namespace rx::net
