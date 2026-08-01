// rpctest: deterministic checks for the typed RPC value, the message codec, and
// the handler registry. No game data, no networking, no framework; it runs in
// the ctest gate. Decode input is treated as hostile, so the codec tests cover
// truncation, a bad magic, an over-limit length, and trailing garbage in
// addition to the exact round trip for every value type.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_registry.h"
#include "rpc/rpc_value.h"

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

using rx::rpc::DecodeCall;
using rx::rpc::EncodeCall;
using rx::rpc::RpcArgs;
using rx::rpc::RpcCall;
using rx::rpc::RpcContext;
using rx::rpc::RpcRegistry;
using rx::rpc::RpcValue;

void TestValue() {
  std::puts("rpc value construction/accessors:");
  RpcValue null;
  Check("default is null", null.is_null());
  Check("default type kNull", null.type() == RpcValue::Type::kNull);

  RpcValue b(true);
  Check("bool type", b.type() == RpcValue::Type::kBool);
  Check("bool value", b.as_bool() == true);

  RpcValue i(rx::i64{-42});
  Check("int type", i.type() == RpcValue::Type::kInt);
  Check("int value", i.as_int() == -42);

  RpcValue f(rx::f64{3.5});
  Check("float type", f.type() == RpcValue::Type::kFloat);
  Check("float value", f.as_float() == 3.5);

  RpcValue s(std::string("hello"));
  Check("string type", s.type() == RpcValue::Type::kString);
  Check("string value", s.as_string() == "hello");

  RpcValue blob(std::vector<rx::u8>{1, 2, 3});
  Check("blob type", blob.type() == RpcValue::Type::kBlob);
  Check("blob value", blob.as_blob() == (std::vector<rx::u8>{1, 2, 3}));

  std::puts("rpc value type-mismatch defaults:");
  Check("bool mismatch default", i.as_bool(true) == true);
  Check("int mismatch default", s.as_int(7) == 7);
  Check("float mismatch default", b.as_float(1.25) == 1.25);
  Check("string mismatch empty", i.as_string().empty());
  Check("blob mismatch empty", s.as_blob().empty());

  std::puts("rpc value equality:");
  Check("equal same", RpcValue(rx::i64{5}) == RpcValue(rx::i64{5}));
  Check("unequal value", !(RpcValue(rx::i64{5}) == RpcValue(rx::i64{6})));
  Check("unequal type", !(RpcValue(rx::i64{1}) == RpcValue(true)));
  Check("null equal null", RpcValue() == RpcValue());
}

// Encodes then decodes and confirms the call is reproduced exactly.
void CheckRoundTrip(const char* what, const RpcCall& call) {
  std::vector<rx::u8> bytes = EncodeCall(call);
  std::optional<RpcCall> back = DecodeCall(bytes.data(), bytes.size());
  bool ok = back.has_value() && back->name == call.name && back->args == call.args;
  Check(what, ok);
}

void TestCodec() {
  std::puts("rpc codec round trip:");
  CheckRoundTrip("null arg", {"a.null", {RpcValue()}});
  CheckRoundTrip("bool arg", {"a.bool", {RpcValue(false)}});
  CheckRoundTrip("int arg", {"a.int", {RpcValue(rx::i64{-1234567890123})}});
  CheckRoundTrip("float arg", {"a.float", {RpcValue(rx::f64{-2.718281828})}});
  CheckRoundTrip("string arg", {"a.string", {RpcValue(std::string("a\0b", 3))}});
  CheckRoundTrip("blob arg",
                 {"a.blob", {RpcValue(std::vector<rx::u8>{0, 255, 16, 32})}});
  CheckRoundTrip("empty args", {"a.noargs", {}});

  RpcCall mixed;
  mixed.name = "spell.cast";
  mixed.args = {RpcValue(rx::i64{7}), RpcValue(std::string("Fireball")),
                RpcValue(rx::f64{1.5}), RpcValue(true), RpcValue(),
                RpcValue(std::vector<rx::u8>{9, 8, 7})};
  CheckRoundTrip("mixed args", mixed);
}

void TestCodecRejects() {
  std::puts("rpc codec rejects hostile input:");
  RpcCall call;
  call.name = "x";
  call.args = {RpcValue(std::string("payload"))};
  std::vector<rx::u8> good = EncodeCall(call);

  Check("good decodes", DecodeCall(good.data(), good.size()).has_value());

  // Truncated at every length short of the full buffer must fail, never crash.
  bool any_truncation_ok = false;
  for (size_t n = 0; n < good.size(); ++n) {
    if (DecodeCall(good.data(), n).has_value()) any_truncation_ok = true;
  }
  Check("all truncations rejected", !any_truncation_ok);

  std::vector<rx::u8> bad_magic = good;
  bad_magic[0] ^= 0xFF;
  Check("bad magic rejected", !DecodeCall(bad_magic.data(), bad_magic.size()).has_value());

  std::vector<rx::u8> trailing = good;
  trailing.push_back(0x00);
  Check("trailing garbage rejected",
        !DecodeCall(trailing.data(), trailing.size()).has_value());

  // An over-limit string length (> 16 MiB) in the header must be rejected before
  // any allocation, even though the buffer itself is tiny.
  std::vector<rx::u8> over;
  auto put_u32 = [&](rx::u32 v) {
    for (int i = 0; i < 4; ++i) over.push_back(rx::u8(v >> (8 * i)));
  };
  auto put_u16 = [&](rx::u16 v) {
    over.push_back(rx::u8(v));
    over.push_back(rx::u8(v >> 8));
  };
  put_u32(rx::FourCc('R', 'P', 'C', '1'));
  put_u16(1);          // name length
  over.push_back('n');
  put_u16(1);          // arg count
  over.push_back(static_cast<rx::u8>(RpcValue::Type::kString));
  put_u32(0x7FFFFFFF);  // ~2 GiB declared length
  Check("over-limit length rejected", !DecodeCall(over.data(), over.size()).has_value());

  // An unknown type tag means the stream is corrupt.
  std::vector<rx::u8> bad_tag;
  bad_tag.insert(bad_tag.end(), over.begin(), over.begin() + 4);  // magic
  bad_tag.push_back(0); bad_tag.push_back(0);  // name length 0
  bad_tag.push_back(1); bad_tag.push_back(0);  // arg count 1
  bad_tag.push_back(200);                      // unknown tag
  Check("unknown type tag rejected",
        !DecodeCall(bad_tag.data(), bad_tag.size()).has_value());
}

void TestRegistry() {
  std::puts("rpc registry dispatch:");
  RpcRegistry reg;
  Check("empty size", reg.size() == 0);
  Check("absent name", !reg.Has("ping"));

  RpcContext seen_ctx;
  RpcArgs seen_args;
  int calls = 0;
  reg.On("ping", [&](const RpcContext& ctx, const RpcArgs& args) {
    seen_ctx = ctx;
    seen_args = args;
    ++calls;
  });
  Check("registered size", reg.size() == 1);
  Check("has registered name", reg.Has("ping"));

  RpcContext ctx;
  ctx.sender = 99;
  ctx.from_server = true;
  RpcCall call{"ping", {RpcValue(rx::i64{5}), RpcValue(std::string("hi"))}};
  Check("dispatch returns true", reg.Dispatch(ctx, call));
  Check("handler invoked once", calls == 1);
  Check("context sender forwarded", seen_ctx.sender == 99);
  Check("context from_server forwarded", seen_ctx.from_server == true);
  Check("args forwarded", seen_args == call.args);

  RpcCall missing{"pong", {}};
  Check("dispatch missing returns false", !reg.Dispatch(ctx, missing));
  Check("missing handler not invoked", calls == 1);

  // On replaces an existing handler rather than adding.
  reg.On("ping", [&](const RpcContext&, const RpcArgs&) { ++calls; });
  Check("replace keeps size", reg.size() == 1);
  reg.Dispatch(ctx, call);
  Check("replacement invoked", calls == 2);

  reg.Clear();
  Check("cleared size", reg.size() == 0);
  Check("cleared has none", !reg.Has("ping"));
}

}  // namespace

int main() {
  TestValue();
  TestCodec();
  TestCodecRejects();
  TestRegistry();
  if (g_failures == 0) {
    std::puts("rpc: all checks passed");
    return 0;
  }
  std::printf("rpc: %d checks FAILED\n", g_failures);
  return 1;
}
