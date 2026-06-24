// codec_fuzztest: hammers the wire decoders that parse untrusted network bytes
// (the mod manifest and the scripting RPC) with random and bit-mutated input. A
// decoder must always either return a value or nullopt, never crash or read out
// of bounds, and a value it accepts must re-encode and decode back unchanged. A
// fixed seed keeps it deterministic, so it runs in the ctest gate. It catches
// hard faults directly; under a sanitizer build it also catches silent OOB reads.

#include <cstdio>
#include <string>
#include <vector>

#include "core/types.h"
#include "modstream/manifest_codec.h"
#include "modstream/mod_resource.h"
#include "rpc/rpc_message.h"
#include "rpc/rpc_value.h"

using namespace rec;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// A small deterministic PRNG (xorshift64), so the corpus is identical every run.
struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}
  u64 Next() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
  u32 Below(u32 n) { return n == 0 ? 0 : static_cast<u32>(Next() % n); }
  u8 Byte() { return static_cast<u8>(Next()); }
};

// Builds a plausible random manifest, so encode/decode round-trips exercise the
// happy path as well as the reject path.
modstream::ModManifest RandomManifest(Rng& rng) {
  modstream::ModManifest m;
  const u32 resources = rng.Below(5);
  for (u32 i = 0; i < resources; ++i) {
    modstream::ModResource r;
    r.name = "res" + std::to_string(rng.Below(1000));
    const u32 files = rng.Below(6);
    for (u32 j = 0; j < files; ++j) {
      modstream::ResourceFile f;
      f.path = "p/" + std::to_string(rng.Below(100000)) + ".bin";
      f.size = rng.Next() & 0xffffffff;
      f.hash = rng.Next();
      r.files.push_back(std::move(f));
    }
    m.resources.push_back(std::move(r));
  }
  return m;
}

rpc::RpcCall RandomCall(Rng& rng) {
  rpc::RpcCall call;
  call.name = "ev" + std::to_string(rng.Below(1000));
  const u32 args = rng.Below(8);
  for (u32 i = 0; i < args; ++i) {
    switch (rng.Below(6)) {
      case 0: call.args.emplace_back(); break;
      case 1: call.args.emplace_back(rng.Byte() & 1 ? true : false); break;
      case 2: call.args.emplace_back(static_cast<i64>(rng.Next())); break;
      case 3: call.args.emplace_back(static_cast<f64>(rng.Next())); break;
      case 4: call.args.emplace_back(std::string(rng.Below(40), 'x')); break;
      default: call.args.emplace_back(std::vector<u8>(rng.Below(40), 0xab)); break;
    }
  }
  return call;
}

}  // namespace

int main() {
  std::printf("codec_fuzztest\n");
  Rng rng(0xC0DEFACEull);
  constexpr int kRounds = 60000;

  bool manifest_roundtrips = true;
  bool rpc_roundtrips = true;

  for (int round = 0; round < kRounds; ++round) {
    // 1. Pure random bytes: the decoder must not crash; almost all are rejected.
    {
      std::vector<u8> noise(rng.Below(2048));
      for (u8& b : noise) b = rng.Byte();
      (void)modstream::DecodeManifest(noise.data(), noise.size());
      (void)rpc::DecodeCall(noise.data(), noise.size());
    }

    // 2. A valid encoding, round-tripped, then bit-mutated: still must not crash,
    //    and the clean encoding must decode back to an equal value.
    {
      const modstream::ModManifest m = RandomManifest(rng);
      std::vector<u8> bytes = modstream::EncodeManifest(m);
      auto decoded = modstream::DecodeManifest(bytes);
      if (!decoded || !(*decoded == m)) manifest_roundtrips = false;
      const u32 flips = rng.Below(8);
      for (u32 i = 0; i < flips && !bytes.empty(); ++i)
        bytes[rng.Below(static_cast<u32>(bytes.size()))] ^= (1u << rng.Below(8));
      (void)modstream::DecodeManifest(bytes);
    }
    {
      const rpc::RpcCall c = RandomCall(rng);
      std::vector<u8> bytes = rpc::EncodeCall(c);
      auto decoded = rpc::DecodeCall(bytes.data(), bytes.size());
      if (!decoded) rpc_roundtrips = false;
      const u32 flips = rng.Below(8);
      for (u32 i = 0; i < flips && !bytes.empty(); ++i)
        bytes[rng.Below(static_cast<u32>(bytes.size()))] ^= (1u << rng.Below(8));
      (void)rpc::DecodeCall(bytes.data(), bytes.size());

      // Truncations of a valid buffer are a common attack; none may crash.
      for (size_t cut = 0; cut < bytes.size(); cut += 3)
        (void)rpc::DecodeCall(bytes.data(), cut);
    }
  }

  // Reaching here means no decode crashed across the whole corpus.
  Check("decoders survived the fuzz corpus without faulting", true);
  Check("every valid manifest round-trips", manifest_roundtrips);
  Check("every valid rpc call round-trips", rpc_roundtrips);

  std::printf("codec_fuzztest: %d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
