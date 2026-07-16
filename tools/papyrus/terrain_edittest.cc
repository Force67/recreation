#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "world/terrain_edits.h"

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << '\n';
  ++failures;
}

std::optional<rx::f32> FlatBase(rx::i32, rx::i32) { return 100.0f; }

rx::u64 Fingerprint(rx::world::TerrainCellKey cell) {
  return 0x9e3779b97f4a7c15ull ^
         (static_cast<rx::u64>(static_cast<rx::u32>(cell.x)) << 32) ^
         static_cast<rx::u32>(cell.y);
}

void FingerprintTouched(rx::world::TerrainEdits* edits) {
  for (rx::world::TerrainCellKey cell : edits->touched_cells())
    edits->SetCellFingerprint(cell, Fingerprint(cell));
}

std::vector<rx::u8> ReadBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<rx::u8>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

void TestCanonicalBordersAndNegativeCells() {
  using namespace rx::world;
  TerrainEdits edits;
  edits.BindWorld("skyrimse:tamriel@skyrim.esm");
  TerrainBrush brush;
  brush.center_x = 32;
  brush.center_y = 5;
  brush.radius = 0.25f;
  brush.strength = 2.0f;
  TerrainEditChange border = edits.ApplyBrush(brush, FlatBase);
  Check(border.samples.size() == 1, "border brush edits one canonical sample");
  Check(border.cells == std::vector<TerrainCellKey>({{0, 0}, {1, 0}}),
        "canonical edge marks both sharing cells");
  Check(edits.SampleDelta(32, 5) == 2.0f,
        "canonical border stores one final delta");

  std::vector<rx::f32> base(33 * 33, 100.0f);
  std::vector<rx::f32> west(33 * 33), east(33 * 33);
  Check(edits.ComposeCell({0, 0}, base, west), "compose west border cell");
  Check(edits.ComposeCell({1, 0}, base, east), "compose east border cell");
  Check(west[5 * 33 + 32] == 102.0f && east[5 * 33] == 102.0f,
        "shared cell vertices compose the same canonical delta");

  TerrainEdits negative;
  negative.BindWorld("negative");
  brush.center_x = -32;
  brush.center_y = -32;
  brush.radius = 0.25f;
  TerrainEditChange corner = negative.ApplyBrush(brush, FlatBase);
  const std::vector<TerrainCellKey> expected = {
      {-2, -2}, {-2, -1}, {-1, -2}, {-1, -1}};
  Check(corner.cells == expected,
        "negative canonical corner reaches all four sharing cells");
  std::vector<rx::f32> southwest(33 * 33);
  Check(negative.ComposeCell({-2, -2}, base, southwest),
        "compose negative cell");
  Check(southwest[32 * 33 + 32] == 102.0f,
        "negative floor division maps shared corner to local 32,32");
}

void TestMergeRevertAndCompose() {
  using namespace rx::world;
  TerrainEdits edits;
  edits.BindWorld("merge");
  TerrainBrush brush;
  brush.center_x = 7;
  brush.center_y = -9;
  brush.radius = 0.25f;
  brush.strength = 1.25f;
  TerrainEditChange stroke = edits.ApplyBrush(brush, FlatBase);
  brush.strength = 0.75f;
  TerrainEditChange dab = edits.ApplyBrush(brush, FlatBase);
  Check(MergeTerrainEditChanges(&stroke, dab), "merge sequential dabs");
  Check(stroke.samples.size() == 1 &&
            stroke.samples[0].old_delta == 0.0f &&
            stroke.samples[0].new_delta == 2.0f,
        "merged stroke keeps first old and final new delta");
  Check(edits.RevertChange(stroke), "revert whole merged stroke");
  Check(edits.sample_count() == 0, "revert removes zero sparse sample");
  Check(edits.ApplyChange(stroke), "reapply whole merged stroke");
  Check(edits.SampleDelta(7, -9) == 2.0f, "reapply restores final delta");

  TerrainEditChange reset = edits.Clear();
  Check(!reset.empty() && edits.sample_count() == 0,
        "clear returns an already-applied reversible change");
  Check(edits.RevertChange(reset) && edits.SampleDelta(7, -9) == 2.0f,
        "reset change can be undone");

  TerrainEditChange out_of_range;
  out_of_range.samples.push_back(
      {{1, 1}, 0.0f, std::numeric_limits<rx::f32>::max()});
  out_of_range.cells.push_back({0, 0});
  Check(!edits.ApplyChange(out_of_range),
        "out-of-range quantized deltas are rejected before mutation");
}

void TestPersistenceValidation() {
  using namespace rx::world;
  TerrainEdits edits;
  const std::string world = "skyrimse:tamriel@skyrim.esm";
  edits.BindWorld(world);
  TerrainBrush brush;
  brush.center_x = -32;
  brush.center_y = 0;
  brush.radius = 2.2f;
  brush.strength = 0.101f;
  Check(!edits.ApplyBrush(brush, FlatBase).empty(),
        "persistence fixture has sparse edits");

  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("recreation-terrain-edittest-" +
       std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root);
  const std::filesystem::path first = root / "first.recterrain";
  const std::filesystem::path second = root / "second.recterrain";
  std::string error;
  Check(!SaveTerrainEdits(edits, first.string(), &error) &&
            error.find("fingerprint") != std::string::npos,
        "saving rejects touched cells without a base fingerprint");
  FingerprintTouched(&edits);
  Check(SaveTerrainEdits(edits, first.string(), &error),
        "save compact terrain diff");
  Check(SaveTerrainEdits(edits, first.string(), &error),
        "save atomically replaces an existing terrain diff");
  Check(SaveTerrainEdits(edits, second.string(), &error),
        "save deterministic terrain diff again");
  const std::vector<rx::u8> bytes = ReadBytes(first);
  Check(bytes == ReadBytes(second), "terrain diff bytes are deterministic");
  Check(bytes.size() < 2048, "sparse terrain diff stays compact");

  const auto valid_fingerprint = [](TerrainCellKey cell) {
    return std::optional<rx::u64>(Fingerprint(cell));
  };
  TerrainEdits loaded;
  Check(LoadTerrainEdits(first.string(), world, valid_fingerprint, &loaded,
                         &error),
        "load quantized terrain diff");
  Check(loaded.sample_count() == edits.sample_count() && !loaded.dirty(),
        "loaded diff preserves samples and starts clean");
  Check(std::abs(loaded.SampleDelta(-32, 0) - edits.SampleDelta(-32, 0)) <=
            1.0f / 256.0f,
        "quantized load retains sub-centimeter engine precision");

  TerrainEdits rejected;
  Check(!LoadTerrainEdits(first.string(), "fallout4:commonwealth@fallout4.esm",
                          valid_fingerprint, &rejected, &error) &&
            error.find("worldspace mismatch") != std::string::npos,
        "wrong worldspace is clearly rejected");
  const auto wrong_fingerprint = [](TerrainCellKey cell) {
    return std::optional<rx::u64>(Fingerprint(cell) + 1);
  };
  Check(!LoadTerrainEdits(first.string(), world, wrong_fingerprint, &rejected,
                          &error) &&
            error.find("fingerprint mismatch") != std::string::npos,
        "changed base source is clearly rejected");

  std::vector<rx::u8> corrupt = bytes;
  corrupt[corrupt.size() / 2] ^= 0x80;
  const std::filesystem::path corrupt_path = root / "corrupt.recterrain";
  WriteBytes(corrupt_path, corrupt);
  Check(!LoadTerrainEdits(corrupt_path.string(), world, valid_fingerprint,
                          &rejected, &error) &&
            error.find("checksum") != std::string::npos,
        "corruption is rejected by checksum");

  corrupt.resize(corrupt.size() / 2);
  const std::filesystem::path truncated_path = root / "truncated.recterrain";
  WriteBytes(truncated_path, corrupt);
  Check(!LoadTerrainEdits(truncated_path.string(), world, valid_fingerprint,
                          &rejected, &error),
        "truncated terrain diff is rejected");

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
  TestCanonicalBordersAndNegativeCells();
  TestMergeRevertAndCompose();
  TestPersistenceValidation();
  if (failures != 0) {
    std::cerr << failures << " terrain edit test(s) failed\n";
    return 1;
  }
  std::cout << "terrain_edittest: ok\n";
  return 0;
}
