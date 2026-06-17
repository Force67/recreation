#include "net/stage_request.h"

#include <nanobuf.h>

namespace rec::net {
namespace {

// The whole request is a single fixed-size little-endian record, so there is no
// length field to validate: a buffer of any other size is rejected outright.
constexpr size_t kWireSize = 8 + 1 + 4 + 4;  // u64 quest | u8 op | i32 a | i32 b

bool IsKnownOp(u8 op) {
  switch (static_cast<StageOp>(op)) {
    case StageOp::kSetStage:
    case StageOp::kSetRunning:
    case StageOp::kSetObjectiveDisplayed:
    case StageOp::kSetObjectiveCompleted:
      return true;
  }
  return false;
}

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

}  // namespace

std::vector<u8> EncodeStageRequest(const StageRequest& req) {
  std::vector<u8> out;
  out.reserve(kWireSize);
  AppendU64(out, req.quest);
  out.push_back(static_cast<u8>(req.op));
  AppendU32(out, static_cast<u32>(req.a));
  AppendU32(out, static_cast<u32>(req.b));
  return out;
}

std::optional<StageRequest> DecodeStageRequest(ByteSpan data) {
  if (data.size() != kWireSize) return std::nullopt;
  const u8* p = data.data();
  if (!IsKnownOp(p[8])) return std::nullopt;

  StageRequest req;
  req.quest = nanobuf::LoadLe<u64>(p);
  req.op = static_cast<StageOp>(p[8]);
  req.a = static_cast<i32>(nanobuf::LoadLe<u32>(p + 9));
  req.b = static_cast<i32>(nanobuf::LoadLe<u32>(p + 13));
  return req;
}

}  // namespace rec::net
