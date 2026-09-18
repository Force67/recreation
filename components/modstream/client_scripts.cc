#include "components/modstream/client_scripts.h"

#include <string>
#include <utility>
#include <vector>

namespace rx::modstream {
namespace {

// A real server stays orders of magnitude under this; the decoder refuses more.
constexpr u16 kMaxPathLen = 4096;

void PutU16(std::vector<u8>& out, u16 v) {
  out.push_back(static_cast<u8>(v));
  out.push_back(static_cast<u8>(v >> 8));
}

void PutU32(std::vector<u8>& out, u32 v) {
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

void PutU64(std::vector<u8>& out, u64 v) {
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}

// Cursor over the received bytes; every read checks bounds first and flips ok
// on underrun, so callers can read optimistically and test ok() once.
class Reader {
 public:
  Reader(const u8* data, size_t size) : data_(data), size_(size) {}

  u32 U32() {
    if (!Need(4))
      return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<u32>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
  }

  u64 U64() {
    if (!Need(8))
      return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<u64>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
  }

  std::string String(u16 max_len) {
    if (!Need(2)) {
      ok_ = false;
      return {};
    }
    const u16 len = static_cast<u16>(data_[pos_]) | static_cast<u16>(data_[pos_ + 1]) << 8;
    pos_ += 2;
    if (len > max_len || !Need(len)) {
      ok_ = false;
      return {};
    }
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
  }

  bool ok() const { return ok_; }
  bool at_end() const { return pos_ == size_; }

 private:
  bool Need(size_t n) {
    if (pos_ + n > size_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const u8* data_;
  size_t size_;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace

std::vector<u8> EncodeClientScripts(u32 generation,
                                    const std::vector<ClientScriptEntry>& scripts) {
  std::vector<u8> out;
  PutU32(out, generation);
  PutU32(out, static_cast<u32>(scripts.size()));
  for (const ClientScriptEntry& entry : scripts) {
    PutU64(out, entry.hash);
    PutU64(out, entry.size);
    PutU16(out, static_cast<u16>(entry.path.size()));
    out.insert(out.end(), entry.path.begin(), entry.path.end());
  }
  return out;
}

std::optional<ClientScriptOffer> DecodeClientScripts(const u8* data,
                                                     size_t size,
                                                     size_t max_entries) {
  Reader r(data, size);
  ClientScriptOffer offer;
  offer.generation = r.U32();
  const u32 count = r.U32();
  if (!r.ok() || count > max_entries)
    return std::nullopt;

  offer.entries.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    ClientScriptEntry entry;
    entry.hash = r.U64();
    entry.size = r.U64();
    entry.path = r.String(kMaxPathLen);
    if (!r.ok())
      return std::nullopt;
    offer.entries.push_back(std::move(entry));
  }

  // A well-formed list consumes the buffer exactly; trailing bytes mean the
  // payload is not what it claims to be.
  if (!r.at_end())
    return std::nullopt;
  return offer;
}

}  // namespace rx::modstream
