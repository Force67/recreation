// deflatetest: deterministic checks for the real DEFLATE encoder ZlibDeflate.
// It round-trips several inputs through ZlibDeflate -> ZlibInflate (which only
// accepts spec-correct zlib streams), confirms the output matches, and proves
// the encoder actually shrinks compressible data. Needs no game data and runs
// in the ctest gate. Modeled on ba2test.cc.

#include <cstdio>
#include <string>

#include "bethesda/compression.h"
#include "core/types.h"

using namespace rec;
using namespace rec::bethesda;

namespace {

int g_failures = 0;

void Check(const char* what, bool ok) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failures;
}

// Deflates `in`, inflates it back into a buffer of the original size, and
// verifies the bytes survive the round trip. Returns the compressed size (or 0
// on failure) so callers can also assert on the compression ratio.
size_t RoundTrip(const char* name, const base::Vector<u8>& in) {
  auto stream = ZlibDeflate(ByteSpan(in.data(), in.size()));

  base::Vector<u8> back;
  back.resize(in.size());
  bool ok = ZlibInflate(ByteSpan(stream.data(), stream.size()),
                        in.empty() ? nullptr : back.data(), in.size());
  bool same = ok;
  for (size_t i = 0; same && i < in.size(); ++i) {
    if (back[i] != in[i]) same = false;
  }
  std::string label = std::string("round-trips: ") + name;
  Check(label.c_str(), same);
  return same ? stream.size() : 0;
}

}  // namespace

int main() {
  std::puts("deflate round trip:");

  // Empty input.
  {
    base::Vector<u8> in;
    RoundTrip("empty", in);
  }

  // Small input.
  {
    base::Vector<u8> in;
    for (u8 c : {'h', 'e', 'l', 'l', 'o'}) in.push_back(c);
    RoundTrip("small \"hello\"", in);
  }

  // Highly repetitive: 10000 bytes of a short repeating pattern. This must
  // compress well below the input size.
  {
    const char* pat = "ABCDEFGH";
    base::Vector<u8> in;
    for (int i = 0; i < 10000; ++i) in.push_back(static_cast<u8>(pat[i % 8]));
    size_t comp = RoundTrip("repetitive 10000", in);
    Check("repetitive compresses < 25% of input", comp != 0 && comp < in.size() / 4);
  }

  // Pseudo-random via a simple LCG: incompressible, so it exercises the literal
  // path and the worst-case size bound (the encoder falls back to a stored
  // block here, which must still be smaller than a Huffman expansion).
  {
    base::Vector<u8> in;
    u32 state = 0x1234567u;
    for (int i = 0; i < 8000; ++i) {
      state = state * 1103515245u + 12345u;
      in.push_back(static_cast<u8>(state >> 16));
    }
    size_t comp = RoundTrip("pseudo-random 8000", in);
    Check("random stays near input size", comp != 0 && comp < in.size() + in.size() / 16);
  }

  // All one byte. Degenerate Huffman: a single distinct literal plus the EOB,
  // and every match is at distance 1, so the distance alphabet has exactly one
  // used symbol. This stresses the single-symbol / forced-complete-code path.
  {
    base::Vector<u8> in;
    for (int i = 0; i < 5000; ++i) in.push_back(0x41);
    size_t comp = RoundTrip("all-same-byte 5000", in);
    Check("all-same compresses hard", comp != 0 && comp < 128);
  }

  // Two distinct bytes alternating. Matches sit at distance 2 (one used distance
  // symbol) and the literal alphabet has two symbols; another degenerate case.
  {
    base::Vector<u8> in;
    for (int i = 0; i < 5000; ++i) in.push_back(static_cast<u8>(i & 1 ? 0x42 : 0x41));
    size_t comp = RoundTrip("two-distinct-byte 5000", in);
    Check("two-byte compresses hard", comp != 0 && comp < 128);
  }

  // English-like text with real redundancy (repeated words), exercises matches
  // at varied distances and lengths. The dynamic-Huffman path should win here;
  // the achieved size is printed and asserted well under a pure fixed-Huffman
  // encoding (fixed spends >=7 bits per literal, so real entropy coding of this
  // low-entropy text clears 45% of the input with plenty of margin).
  {
    const char* sentence =
        "the quick brown fox jumps over the lazy dog. "
        "the dog was not amused, but the fox jumped again. ";
    base::Vector<u8> in;
    for (int rep = 0; rep < 200; ++rep) {
      for (const char* p = sentence; *p; ++p) in.push_back(static_cast<u8>(*p));
    }
    size_t comp = RoundTrip("english-like text", in);
    std::printf("  english-like text: %zu -> %zu bytes (%.1f%%)\n", static_cast<size_t>(in.size()),
                comp, comp * 100.0 / static_cast<double>(in.size()));
    Check("text compresses below 45% (dynamic Huffman)", comp != 0 && comp < in.size() * 45 / 100);
  }

  // Larger (~20KB) text built from a word list via an LCG, less regular than the
  // repeated sentence so matches and literal frequencies vary more. Exercises
  // the dynamic path over a bigger, more realistic literal/length histogram.
  {
    const char* words[] = {"the",  "quick",  "brown", "fox",   "and",    "dog",    "run",
                           "jump", "over",   "lazy",  "river", "forest", "under",  "bright",
                           "moon", "light",  "again", "while", "water",  "stone"};
    base::Vector<u8> in;
    u32 state = 0x9e3779b9u;
    while (in.size() < 20000) {
      state = state * 1103515245u + 12345u;
      const char* w = words[(state >> 16) % 20];
      for (const char* p = w; *p; ++p) in.push_back(static_cast<u8>(*p));
      in.push_back(static_cast<u8>(' '));
    }
    size_t comp = RoundTrip("english-like 20KB", in);
    std::printf("  english-like 20KB: %zu -> %zu bytes (%.1f%%)\n", static_cast<size_t>(in.size()),
                comp, comp * 100.0 / static_cast<double>(in.size()));
    Check("20KB text compresses below 60%", comp != 0 && comp < in.size() * 60 / 100);
  }

  // Regression guard: the stored (uncompressed) encoder must still round-trip.
  {
    base::Vector<u8> in;
    for (int i = 0; i < 1000; ++i) in.push_back(static_cast<u8>(i * 7 + 3));
    auto stream = ZlibDeflateStored(ByteSpan(in.data(), in.size()));
    base::Vector<u8> back;
    back.resize(in.size());
    bool ok = ZlibInflate(ByteSpan(stream.data(), stream.size()), back.data(), in.size());
    bool same = ok;
    for (size_t i = 0; same && i < in.size(); ++i) {
      if (back[i] != in[i]) same = false;
    }
    Check("ZlibDeflateStored still round-trips", same);
  }

  if (g_failures == 0) {
    std::puts("deflate: all checks passed");
    return 0;
  }
  std::printf("deflate: %d checks FAILED\n", g_failures);
  return 1;
}
