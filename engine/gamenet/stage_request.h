#ifndef RECREATION_NET_STAGE_REQUEST_H_
#define RECREATION_NET_STAGE_REQUEST_H_

#include <optional>
#include <vector>

#include "core/types.h"

namespace rx::net {

// A client's quest-debugger action, sent to the server so the change is applied
// authoritatively and then replicated back through the normal QuestUpdate path.
// Clients never mutate quest state locally; they ask, the server decides.
enum class StageOp : u8 {
  kSetStage = 0,
  kSetRunning = 1,
  kSetObjectiveDisplayed = 2,
  kSetObjectiveCompleted = 3,
};

struct StageRequest {
  u64 quest = 0;  // packed GlobalFormId quest handle
  StageOp op = StageOp::kSetStage;
  i32 a = 0;  // kSetStage: stage index. objective ops: objective index.
  i32 b = 0;  // kSetRunning / objective ops: boolean flag (0/1).
};

// Fixed 17-byte little-endian record: u64 quest | u8 op | i32 a | i32 b.
std::vector<u8> EncodeStageRequest(const StageRequest& req);

// Inverse of EncodeStageRequest. Returns nullopt on a buffer of the wrong size
// or carrying an unknown op value, never reading out of bounds.
std::optional<StageRequest> DecodeStageRequest(ByteSpan data);

}  // namespace rx::net

#endif  // RECREATION_NET_STAGE_REQUEST_H_
