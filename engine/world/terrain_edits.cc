#include "world/terrain_edits.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rx::world {
namespace {

constexpr i64 kCellQuads = 32;
constexpr size_t kCellSamples = 33 * 33;
constexpr i32 kQuantization = 256;  // 1/256 game unit (0.0056 cm in Skyrim).
constexpr i32 kMaximumQuantized =
    (std::numeric_limits<i32>::max() / kQuantization) * kQuantization;
constexpr f32 kMaximumDelta = static_cast<f32>(kMaximumQuantized) / kQuantization;
constexpr f32 kMaximumBrushRadius = 1024.0f;
constexpr u32 kMaximumSamples = 4u * 1024u * 1024u;
constexpr u32 kMaximumCells = 256u * 1024u;
constexpr u32 kMaximumWorldBytes = 1024;
constexpr u64 kMaximumFileBytes = 128ull * 1024ull * 1024ull;
constexpr u16 kFormatVersion = 1;
constexpr u16 kHeaderBytes = 40;
constexpr char kMagic[8] = {'R', 'E', 'C', 'T', 'E', 'R', 'R', '\0'};

i64 FloorDiv(i64 value, i64 divisor) {
  i64 quotient = value / divisor;
  if (value % divisor < 0) --quotient;
  return quotient;
}

i64 PositiveMod(i64 value, i64 divisor) {
  const i64 result = value % divisor;
  return result < 0 ? result + divisor : result;
}

bool IsSortedUnique(const std::vector<TerrainCellKey>& cells) {
  return std::adjacent_find(cells.begin(), cells.end(),
                            [](TerrainCellKey a, TerrainCellKey b) {
                              return !(a < b);
                            }) == cells.end();
}

bool IsSortedUnique(const std::vector<TerrainSampleChange>& samples) {
  return std::adjacent_find(
             samples.begin(), samples.end(),
             [](const TerrainSampleChange& a, const TerrainSampleChange& b) {
               return !(a.sample < b.sample);
             }) == samples.end();
}

std::vector<TerrainCellKey> SharedCells(TerrainSampleKey sample) {
  const i64 owner_x = FloorDiv(sample.x, kCellQuads);
  const i64 owner_y = FloorDiv(sample.y, kCellQuads);
  const bool edge_x = PositiveMod(sample.x, kCellQuads) == 0;
  const bool edge_y = PositiveMod(sample.y, kCellQuads) == 0;
  std::vector<TerrainCellKey> result;
  result.reserve((edge_x ? 2 : 1) * (edge_y ? 2 : 1));
  for (i64 y : {owner_y, owner_y - 1}) {
    if (y != owner_y && !edge_y) continue;
    for (i64 x : {owner_x, owner_x - 1}) {
      if (x != owner_x && !edge_x) continue;
      if (x < std::numeric_limits<i32>::min() ||
          x > std::numeric_limits<i32>::max() ||
          y < std::numeric_limits<i32>::min() ||
          y > std::numeric_limits<i32>::max()) {
        continue;
      }
      result.push_back({static_cast<i32>(x), static_cast<i32>(y)});
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void CollectCells(TerrainEditChange* change) {
  std::set<TerrainCellKey> cells;
  for (const TerrainSampleChange& sample : change->samples) {
    const std::vector<TerrainCellKey> shared = SharedCells(sample.sample);
    cells.insert(shared.begin(), shared.end());
  }
  change->cells.assign(cells.begin(), cells.end());
}

std::optional<f32> QuantizeDelta(double value) {
  if (!std::isfinite(value) || std::abs(value) > kMaximumDelta) {
    return std::nullopt;
  }
  const double quantized = std::round(value * kQuantization) / kQuantization;
  if (!std::isfinite(quantized) || std::abs(quantized) > kMaximumDelta) {
    return std::nullopt;
  }
  return static_cast<f32>(quantized);
}

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

void AppendU16(std::vector<u8>* bytes, u16 value) {
  bytes->push_back(static_cast<u8>(value));
  bytes->push_back(static_cast<u8>(value >> 8));
}

void AppendU32(std::vector<u8>* bytes, u32 value) {
  for (u32 shift = 0; shift < 32; shift += 8)
    bytes->push_back(static_cast<u8>(value >> shift));
}

void AppendI32(std::vector<u8>* bytes, i32 value) {
  AppendU32(bytes, static_cast<u32>(value));
}

void AppendU64(std::vector<u8>* bytes, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8)
    bytes->push_back(static_cast<u8>(value >> shift));
}

u32 Crc32(std::span<const u8> bytes) {
  u32 crc = 0xffffffffu;
  for (u8 byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

class Reader {
 public:
  explicit Reader(std::span<const u8> bytes) : bytes_(bytes) {}

  bool ReadU8(u8* value) {
    if (offset_ == bytes_.size()) return false;
    *value = bytes_[offset_++];
    return true;
  }
  bool ReadU16(u16* value) {
    if (!Has(2)) return false;
    *value = static_cast<u16>(bytes_[offset_]) |
             static_cast<u16>(bytes_[offset_ + 1]) << 8;
    offset_ += 2;
    return true;
  }
  bool ReadU32(u32* value) {
    if (!Has(4)) return false;
    *value = 0;
    for (u32 shift = 0; shift < 32; shift += 8)
      *value |= static_cast<u32>(bytes_[offset_++]) << shift;
    return true;
  }
  bool ReadI32(i32* value) {
    u32 bits = 0;
    if (!ReadU32(&bits)) return false;
    *value = static_cast<i32>(bits);
    return true;
  }
  bool ReadU64(u64* value) {
    if (!Has(8)) return false;
    *value = 0;
    for (u32 shift = 0; shift < 64; shift += 8)
      *value |= static_cast<u64>(bytes_[offset_++]) << shift;
    return true;
  }
  bool ReadString(size_t size, std::string* value) {
    if (!Has(size)) return false;
    value->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  size_t offset() const { return offset_; }
  size_t remaining() const { return bytes_.size() - offset_; }

 private:
  bool Has(size_t size) const { return size <= bytes_.size() - offset_; }

  std::span<const u8> bytes_;
  size_t offset_ = 0;
};

struct EncodedSample {
  u8 x = 0;
  u8 y = 0;
  i32 delta = 0;
};

struct EncodedCell {
  TerrainCellKey key;
  u64 fingerprint = 0;
  std::vector<EncodedSample> samples;
};

}  // namespace

void TerrainEdits::BindWorld(std::string identity) {
  if (identity == world_identity_) return;
  world_identity_ = std::move(identity);
  samples_.clear();
  fingerprints_.clear();
  dirty_cells_.clear();
  dirty_ = false;
}

f32 TerrainEdits::SampleDelta(i32 global_x, i32 global_y) const {
  const auto found = samples_.find({global_x, global_y});
  return found == samples_.end() ? 0.0f : found->second;
}

bool TerrainEdits::AffectsCell(TerrainCellKey cell) const {
  for (i32 row = 0; row <= kCellQuads; ++row) {
    for (i32 col = 0; col <= kCellQuads; ++col) {
      const i64 global_x = static_cast<i64>(cell.x) * kCellQuads + col;
      const i64 global_y = static_cast<i64>(cell.y) * kCellQuads + row;
      if (global_x < std::numeric_limits<i32>::min() ||
          global_x > std::numeric_limits<i32>::max() ||
          global_y < std::numeric_limits<i32>::min() ||
          global_y > std::numeric_limits<i32>::max()) {
        continue;
      }
      if (samples_.contains(
              {static_cast<i32>(global_x), static_cast<i32>(global_y)})) {
        return true;
      }
    }
  }
  return false;
}

bool TerrainEdits::ComposeCell(TerrainCellKey cell,
                               std::span<const f32> base_heights,
                               std::span<f32> composed_heights) const {
  if (base_heights.size() != kCellSamples ||
      composed_heights.size() != kCellSamples) {
    return false;
  }
  for (i32 row = 0; row <= kCellQuads; ++row) {
    for (i32 col = 0; col <= kCellQuads; ++col) {
      const size_t index = static_cast<size_t>(row) * 33 + col;
      const i64 global_x = static_cast<i64>(cell.x) * kCellQuads + col;
      const i64 global_y = static_cast<i64>(cell.y) * kCellQuads + row;
      if (!std::isfinite(base_heights[index]) ||
          global_x < std::numeric_limits<i32>::min() ||
          global_x > std::numeric_limits<i32>::max() ||
          global_y < std::numeric_limits<i32>::min() ||
          global_y > std::numeric_limits<i32>::max()) {
        return false;
      }
      const double height =
          static_cast<double>(base_heights[index]) +
          SampleDelta(static_cast<i32>(global_x), static_cast<i32>(global_y));
      if (!std::isfinite(height) ||
          std::abs(height) > std::numeric_limits<f32>::max()) {
        return false;
      }
      composed_heights[index] = static_cast<f32>(height);
    }
  }
  return true;
}

TerrainEditChange TerrainEdits::ApplyBrush(const TerrainBrush& brush,
                                            const BaseHeight& base_height) {
  TerrainEditChange change;
  if (!base_height || !std::isfinite(brush.center_x) ||
      !std::isfinite(brush.center_y) || !std::isfinite(brush.radius) ||
      brush.radius <= 0 || brush.radius > kMaximumBrushRadius ||
      !std::isfinite(brush.strength) || brush.strength <= 0 ||
      !std::isfinite(brush.falloff) || brush.falloff < 0 ||
      (brush.mode == TerrainBrushMode::kFlatten &&
       !std::isfinite(brush.flatten_target))) {
    return change;
  }

  const double min_x = std::ceil(static_cast<double>(brush.center_x) - brush.radius);
  const double max_x = std::floor(static_cast<double>(brush.center_x) + brush.radius);
  const double min_y = std::ceil(static_cast<double>(brush.center_y) - brush.radius);
  const double max_y = std::floor(static_cast<double>(brush.center_y) + brush.radius);
  // Keep one coordinate of headroom for smooth-neighbor probes and loop
  // increments; this also avoids signed overflow at the lattice extremes.
  if (min_x <= std::numeric_limits<i32>::min() ||
      max_x >= std::numeric_limits<i32>::max() ||
      min_y <= std::numeric_limits<i32>::min() ||
      max_y >= std::numeric_limits<i32>::max()) {
    return change;
  }

  auto current_height = [&](i32 x, i32 y) -> std::optional<f32> {
    std::optional<f32> base = base_height(x, y);
    if (!base || !std::isfinite(*base)) return std::nullopt;
    const double current = static_cast<double>(*base) + SampleDelta(x, y);
    if (!std::isfinite(current) ||
        std::abs(current) > std::numeric_limits<f32>::max()) {
      return std::nullopt;
    }
    return static_cast<f32>(current);
  };

  for (i32 x = static_cast<i32>(min_x); x <= static_cast<i32>(max_x); ++x) {
    for (i32 y = static_cast<i32>(min_y); y <= static_cast<i32>(max_y); ++y) {
      const f32 distance = static_cast<f32>(std::hypot(
          static_cast<double>(x) - brush.center_x,
          static_cast<double>(y) - brush.center_y));
      if (distance > brush.radius) continue;
      const f32 radial = std::max(0.0f, 1.0f - distance / brush.radius);
      const f32 influence = brush.falloff == 0
                                ? 1.0f
                                : std::pow(radial, brush.falloff);
      if (influence <= 0) continue;

      const std::optional<f32> base = base_height(x, y);
      if (!base || !std::isfinite(*base)) continue;
      const f32 old_delta = SampleDelta(x, y);
      const f32 old_height = *base + old_delta;
      double new_delta = old_delta;
      switch (brush.mode) {
        case TerrainBrushMode::kRaise:
          new_delta += brush.strength * influence;
          break;
        case TerrainBrushMode::kLower:
          new_delta -= brush.strength * influence;
          break;
        case TerrainBrushMode::kSmooth: {
          f32 total = 0;
          u32 count = 0;
          for (const TerrainSampleKey neighbor : {
                   TerrainSampleKey{x - 1, y}, TerrainSampleKey{x + 1, y},
                   TerrainSampleKey{x, y - 1}, TerrainSampleKey{x, y + 1}}) {
            if (const std::optional<f32> value =
                    current_height(neighbor.x, neighbor.y)) {
              total += *value;
              ++count;
            }
          }
          if (count == 0) continue;
          const f32 amount =
              std::clamp(brush.strength * influence, 0.0f, 1.0f);
          new_delta += (total / count - old_height) * amount;
          break;
        }
        case TerrainBrushMode::kFlatten: {
          const f32 amount =
              std::clamp(brush.strength * influence, 0.0f, 1.0f);
          new_delta += (brush.flatten_target - old_height) * amount;
          break;
        }
      }
      const std::optional<f32> quantized = QuantizeDelta(new_delta);
      if (!quantized || *quantized == old_delta) continue;
      change.samples.push_back({{x, y}, old_delta, *quantized});
    }
  }
  CollectCells(&change);
  if (!change.empty() && !ApplyChange(change)) return {};
  return change;
}

bool TerrainEdits::SetChangeState(const TerrainEditChange& change,
                                  bool use_new) {
  if (!IsSortedUnique(change.samples) || !IsSortedUnique(change.cells)) {
    return false;
  }
  for (const TerrainSampleChange& sample : change.samples) {
    const f32 expected = use_new ? sample.old_delta : sample.new_delta;
    const f32 wanted = use_new ? sample.new_delta : sample.old_delta;
    if (!std::isfinite(expected) || !std::isfinite(wanted) ||
        std::abs(wanted) > kMaximumDelta ||
        SampleDelta(sample.sample.x, sample.sample.y) != expected) {
      return false;
    }
    const std::vector<TerrainCellKey> shared = SharedCells(sample.sample);
    for (TerrainCellKey cell : shared) {
      if (!std::binary_search(change.cells.begin(), change.cells.end(), cell)) {
        return false;
      }
    }
  }

  for (const TerrainSampleChange& sample : change.samples) {
    const f32 value = use_new ? sample.new_delta : sample.old_delta;
    if (value == 0)
      samples_.erase(sample.sample);
    else
      samples_[sample.sample] = value;
  }
  for (TerrainCellKey cell : change.cells) dirty_cells_[cell] = true;
  if (!change.empty()) dirty_ = true;
  return true;
}

bool TerrainEdits::ApplyChange(const TerrainEditChange& change) {
  return SetChangeState(change, true);
}

bool TerrainEdits::RevertChange(const TerrainEditChange& change) {
  return SetChangeState(change, false);
}

TerrainEditChange TerrainEdits::Clear() {
  TerrainEditChange change;
  change.samples.reserve(samples_.size());
  for (const auto& [sample, delta] : samples_)
    change.samples.push_back({sample, delta, 0.0f});
  CollectCells(&change);
  if (!change.empty() && !ApplyChange(change)) return {};
  return change;
}

void TerrainEdits::SetCellFingerprint(TerrainCellKey cell, u64 fingerprint) {
  fingerprints_[cell] = fingerprint;
}

std::optional<u64> TerrainEdits::CellFingerprint(TerrainCellKey cell) const {
  const auto found = fingerprints_.find(cell);
  if (found == fingerprints_.end()) return std::nullopt;
  return found->second;
}

std::vector<TerrainCellKey> TerrainEdits::dirty_cells() const {
  std::vector<TerrainCellKey> result;
  result.reserve(dirty_cells_.size());
  for (const auto& [cell, ignored] : dirty_cells_) {
    (void)ignored;
    result.push_back(cell);
  }
  return result;
}

std::vector<TerrainCellKey> TerrainEdits::touched_cells() const {
  std::set<TerrainCellKey> touched;
  for (const auto& [sample, ignored] : samples_) {
    (void)ignored;
    const std::vector<TerrainCellKey> shared = SharedCells(sample);
    touched.insert(shared.begin(), shared.end());
  }
  return {touched.begin(), touched.end()};
}

void TerrainEdits::MarkSaved() {
  dirty_ = false;
  dirty_cells_.clear();
}

bool MergeTerrainEditChanges(TerrainEditChange* stroke,
                             const TerrainEditChange& dab) {
  if (!stroke || !IsSortedUnique(dab.samples) || !IsSortedUnique(dab.cells)) {
    return false;
  }
  if (dab.empty()) return true;
  if (stroke->empty()) {
    *stroke = dab;
    return true;
  }
  if (!IsSortedUnique(stroke->samples) || !IsSortedUnique(stroke->cells)) {
    return false;
  }

  TerrainEditChange merged;
  merged.samples.reserve(stroke->samples.size() + dab.samples.size());
  size_t old_index = 0;
  size_t dab_index = 0;
  while (old_index < stroke->samples.size() || dab_index < dab.samples.size()) {
    if (dab_index == dab.samples.size() ||
        (old_index < stroke->samples.size() &&
         stroke->samples[old_index].sample < dab.samples[dab_index].sample)) {
      merged.samples.push_back(stroke->samples[old_index++]);
    } else if (old_index == stroke->samples.size() ||
               dab.samples[dab_index].sample <
                   stroke->samples[old_index].sample) {
      merged.samples.push_back(dab.samples[dab_index++]);
    } else {
      const TerrainSampleChange& old = stroke->samples[old_index++];
      const TerrainSampleChange& later = dab.samples[dab_index++];
      if (old.new_delta != later.old_delta) return false;
      if (old.old_delta != later.new_delta) {
        merged.samples.push_back(
            {old.sample, old.old_delta, later.new_delta});
      }
    }
  }
  CollectCells(&merged);
  *stroke = std::move(merged);
  return true;
}

bool SaveTerrainEdits(const TerrainEdits& edits, const std::string& file_path,
                      std::string* error) {
  if (edits.world_identity_.empty() ||
      edits.world_identity_.size() > kMaximumWorldBytes) {
    SetError(error, "terrain diff has no valid worldspace identity");
    return false;
  }

  std::map<TerrainCellKey, EncodedCell> cells;
  u32 sample_count = 0;
  for (const auto& [sample, delta] : edits.samples_) {
    const double scaled = static_cast<double>(delta) * kQuantization;
    if (!std::isfinite(delta) || std::abs(delta) > kMaximumDelta ||
        scaled < std::numeric_limits<i32>::min() ||
        scaled > std::numeric_limits<i32>::max()) {
      SetError(error, "terrain diff contains a non-finite or out-of-range delta");
      return false;
    }
    const i32 quantized = static_cast<i32>(std::llround(scaled));
    if (quantized == 0) continue;
    const i64 owner_x = FloorDiv(sample.x, kCellQuads);
    const i64 owner_y = FloorDiv(sample.y, kCellQuads);
    if (owner_x < std::numeric_limits<i32>::min() ||
        owner_x > std::numeric_limits<i32>::max() ||
        owner_y < std::numeric_limits<i32>::min() ||
        owner_y > std::numeric_limits<i32>::max()) {
      SetError(error, "terrain sample lies outside the supported cell range");
      return false;
    }
    TerrainCellKey owner{static_cast<i32>(owner_x), static_cast<i32>(owner_y)};
    EncodedCell& encoded = cells[owner];
    encoded.key = owner;
    encoded.samples.push_back(
        {static_cast<u8>(PositiveMod(sample.x, kCellQuads)),
         static_cast<u8>(PositiveMod(sample.y, kCellQuads)), quantized});
    for (TerrainCellKey shared : SharedCells(sample)) cells[shared].key = shared;
    if (++sample_count > kMaximumSamples) {
      SetError(error, "terrain diff exceeds the sample limit");
      return false;
    }
  }
  if (cells.size() > kMaximumCells) {
    SetError(error, "terrain diff exceeds the cell limit");
    return false;
  }
  for (auto& [key, cell] : cells) {
    if (const auto found = edits.fingerprints_.find(key);
        found != edits.fingerprints_.end()) {
      cell.fingerprint = found->second;
    }
    if (cell.fingerprint == 0) {
      SetError(error, "terrain diff is missing a base fingerprint at cell " +
                          std::to_string(key.x) + "," + std::to_string(key.y));
      return false;
    }
  }

  const u64 payload_bytes = edits.world_identity_.size() +
                            cells.size() * 20ull + sample_count * 6ull;
  const u64 total_bytes = kHeaderBytes + payload_bytes + 4;
  if (total_bytes > kMaximumFileBytes) {
    SetError(error, "terrain diff exceeds the file-size limit");
    return false;
  }

  std::vector<u8> bytes;
  bytes.reserve(static_cast<size_t>(total_bytes));
  bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
  AppendU16(&bytes, kFormatVersion);
  AppendU16(&bytes, kHeaderBytes);
  AppendU32(&bytes, static_cast<u32>(edits.world_identity_.size()));
  AppendU32(&bytes, static_cast<u32>(cells.size()));
  AppendU32(&bytes, sample_count);
  AppendU32(&bytes, kQuantization);
  AppendU32(&bytes, 0);
  AppendU64(&bytes, payload_bytes);
  bytes.insert(bytes.end(), edits.world_identity_.begin(),
               edits.world_identity_.end());
  for (const auto& [key, cell] : cells) {
    AppendI32(&bytes, key.x);
    AppendI32(&bytes, key.y);
    AppendU64(&bytes, cell.fingerprint);
    AppendU32(&bytes, static_cast<u32>(cell.samples.size()));
    for (const EncodedSample& sample : cell.samples) {
      bytes.push_back(sample.x);
      bytes.push_back(sample.y);
      AppendI32(&bytes, sample.delta);
    }
  }
  AppendU32(&bytes, Crc32(bytes));

  const std::filesystem::path destination(file_path);
  const std::filesystem::path temporary = destination.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      SetError(error, "cannot open terrain diff temporary file for writing");
      return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      output.close();
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      SetError(error, "failed while writing terrain diff temporary file");
      return false;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, destination, rename_error);
#if defined(_WIN32)
  if (rename_error &&
      MoveFileExW(temporary.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    rename_error.clear();
  }
#endif
  if (rename_error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    SetError(error, "cannot replace terrain diff: " + rename_error.message());
    return false;
  }
  if (error) error->clear();
  return true;
}

bool LoadTerrainEdits(const std::string& file_path,
                      std::string_view expected_world_identity,
                      const TerrainEdits::FingerprintLookup& fingerprints,
                      TerrainEdits* edits, std::string* error) {
  if (!edits || expected_world_identity.empty() || !fingerprints) {
    SetError(error, "terrain diff load has no destination, worldspace, or fingerprint source");
    return false;
  }
  std::ifstream input(file_path, std::ios::binary | std::ios::ate);
  if (!input) {
    SetError(error, "cannot open terrain diff");
    return false;
  }
  const std::streamoff length = input.tellg();
  if (length < static_cast<std::streamoff>(kHeaderBytes + 4) ||
      static_cast<u64>(length) > kMaximumFileBytes) {
    SetError(error, "terrain diff is truncated or exceeds the file-size limit");
    return false;
  }
  input.seekg(0);
  std::vector<u8> bytes(static_cast<size_t>(length));
  input.read(reinterpret_cast<char*>(bytes.data()), length);
  if (!input) {
    SetError(error, "failed while reading terrain diff");
    return false;
  }
  u32 stored_checksum = 0;
  const size_t checksum_offset = bytes.size() - 4;
  for (u32 shift = 0; shift < 32; shift += 8)
    stored_checksum |= static_cast<u32>(bytes[checksum_offset + shift / 8])
                       << shift;
  if (stored_checksum !=
      Crc32(std::span<const u8>(bytes.data(), checksum_offset))) {
    SetError(error, "terrain diff checksum mismatch");
    return false;
  }

  Reader reader(std::span<const u8>(bytes.data(), checksum_offset));
  for (char expected : kMagic) {
    u8 actual = 0;
    if (!reader.ReadU8(&actual) || actual != static_cast<u8>(expected)) {
      SetError(error, "terrain diff has an invalid magic value");
      return false;
    }
  }
  u16 version = 0, header_bytes = 0;
  u32 world_bytes = 0, cell_count = 0, sample_count = 0;
  u32 quantization = 0, reserved = 0;
  u64 payload_bytes = 0;
  if (!reader.ReadU16(&version) || !reader.ReadU16(&header_bytes) ||
      !reader.ReadU32(&world_bytes) || !reader.ReadU32(&cell_count) ||
      !reader.ReadU32(&sample_count) || !reader.ReadU32(&quantization) ||
      !reader.ReadU32(&reserved) || !reader.ReadU64(&payload_bytes)) {
    SetError(error, "terrain diff header is truncated");
    return false;
  }
  if (version != kFormatVersion || header_bytes != kHeaderBytes ||
      quantization != kQuantization || reserved != 0 || world_bytes == 0 ||
      world_bytes > kMaximumWorldBytes || cell_count > kMaximumCells ||
      sample_count > kMaximumSamples || payload_bytes != reader.remaining()) {
    SetError(error, "terrain diff header contains unsupported or invalid values");
    return false;
  }
  std::string world_identity;
  if (!reader.ReadString(world_bytes, &world_identity)) {
    SetError(error, "terrain diff worldspace identity is truncated");
    return false;
  }
  if (world_identity != expected_world_identity) {
    SetError(error, "terrain diff worldspace mismatch: file is bound to '" +
                        world_identity + "', active world is '" +
                        std::string(expected_world_identity) + "'");
    return false;
  }

  TerrainEdits loaded;
  loaded.world_identity_ = world_identity;
  TerrainCellKey previous_cell;
  bool has_previous_cell = false;
  u32 decoded_samples = 0;
  for (u32 cell_index = 0; cell_index < cell_count; ++cell_index) {
    TerrainCellKey cell;
    u64 fingerprint = 0;
    u32 cell_samples = 0;
    if (!reader.ReadI32(&cell.x) || !reader.ReadI32(&cell.y) ||
        !reader.ReadU64(&fingerprint) || !reader.ReadU32(&cell_samples) ||
        cell_samples > sample_count - decoded_samples) {
      SetError(error, "terrain diff cell table is truncated or invalid");
      return false;
    }
    if (has_previous_cell && !(previous_cell < cell)) {
      SetError(error, "terrain diff cells are not strictly sorted");
      return false;
    }
    previous_cell = cell;
    has_previous_cell = true;
    if (fingerprint == 0) {
      SetError(error, "terrain diff cell has no base fingerprint");
      return false;
    }
    const std::optional<u64> active = fingerprints(cell);
    if (!active || *active != fingerprint) {
      SetError(error, "terrain diff base fingerprint mismatch at cell " +
                          std::to_string(cell.x) + "," +
                          std::to_string(cell.y));
      return false;
    }
    loaded.fingerprints_[cell] = fingerprint;
    TerrainSampleKey previous_sample;
    bool has_previous_sample = false;
    for (u32 sample_index = 0; sample_index < cell_samples; ++sample_index) {
      u8 local_x = 0, local_y = 0;
      i32 quantized = 0;
      if (!reader.ReadU8(&local_x) || !reader.ReadU8(&local_y) ||
          !reader.ReadI32(&quantized) || local_x >= kCellQuads ||
          local_y >= kCellQuads || quantized == 0 ||
          quantized < -kMaximumQuantized || quantized > kMaximumQuantized) {
        SetError(error, "terrain diff sample table is truncated or invalid");
        return false;
      }
      const i64 global_x = static_cast<i64>(cell.x) * kCellQuads + local_x;
      const i64 global_y = static_cast<i64>(cell.y) * kCellQuads + local_y;
      if (global_x < std::numeric_limits<i32>::min() ||
          global_x > std::numeric_limits<i32>::max() ||
          global_y < std::numeric_limits<i32>::min() ||
          global_y > std::numeric_limits<i32>::max()) {
        SetError(error, "terrain diff sample coordinate overflows the lattice");
        return false;
      }
      TerrainSampleKey sample{static_cast<i32>(global_x),
                              static_cast<i32>(global_y)};
      if (has_previous_sample && !(previous_sample < sample)) {
        SetError(error, "terrain diff samples are not strictly sorted");
        return false;
      }
      previous_sample = sample;
      has_previous_sample = true;
      if (!loaded.samples_
               .emplace(sample,
                        static_cast<f32>(quantized) / kQuantization)
               .second) {
        SetError(error, "terrain diff contains duplicate samples");
        return false;
      }
      ++decoded_samples;
    }
  }
  if (decoded_samples != sample_count || reader.remaining() != 0) {
    SetError(error, "terrain diff counts or trailing length are invalid");
    return false;
  }
  for (const auto& [sample, ignored] : loaded.samples_) {
    (void)ignored;
    for (TerrainCellKey shared : SharedCells(sample)) {
      if (!loaded.fingerprints_.contains(shared)) {
        SetError(error, "terrain diff omits a cell sharing an edited border");
        return false;
      }
    }
  }
  loaded.MarkSaved();
  *edits = std::move(loaded);
  if (error) error->clear();
  return true;
}

}  // namespace rx::world
