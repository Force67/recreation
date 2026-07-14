#include <cstdio>
#include <filesystem>
#include <string>

#include "bethesda/game_profile.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/writer.h"
#include "script/games/skyrim/skyrim_bindings.h"
#include "world/prop_streaming.h"

namespace {

constexpr rx::u32 FourCc(char a, char b, char c, char d) {
  return static_cast<rx::u32>(a) | (static_cast<rx::u32>(b) << 8) |
         (static_cast<rx::u32>(c) << 16) | (static_cast<rx::u32>(d) << 24);
}

int failures = 0;

void Check(const char* name, bool condition) {
  std::printf("  [%s] %s\n", condition ? "ok" : "FAIL", name);
  if (!condition) ++failures;
}

}  // namespace

int main() {
  using namespace rx::world;

  std::puts("prop streaming classification:");
  Check("plain static batches", ClassifyProp({.base_type = FourCc('S', 'T', 'A', 'T')}).batchable);
  Check("plain tree batches", ClassifyProp({.base_type = FourCc('T', 'R', 'E', 'E')}).batchable);
  Check("placed script stays ECS",
        !ClassifyProp({.base_type = FourCc('S', 'T', 'A', 'T'), .placed_script = true}).batchable);
  Check("base script stays ECS",
        !ClassifyProp({.base_type = FourCc('S', 'T', 'A', 'T'), .base_script = true}).batchable);
  Check("trigger primitive stays ECS",
        !ClassifyProp({.base_type = FourCc('S', 'T', 'A', 'T'), .primitive = true}).batchable);
  Check("stateful static stays ECS",
        !ClassifyProp({.base_type = FourCc('S', 'T', 'A', 'T'), .stateful = true}).batchable);

  const PropClassification door = ClassifyProp({.base_type = FourCc('D', 'O', 'O', 'R')});
  Check("door stays ECS", !door.batchable);
  Check("door is activatable", (door.capabilities & kPropActivatable) != 0);
  Check("door carries door behavior", (door.capabilities & kPropDoor) != 0);

  const PropClassification container = ClassifyProp({.base_type = FourCc('C', 'O', 'N', 'T')});
  Check("container stays ECS", !container.batchable);
  Check("container behavior retained", (container.capabilities & kPropContainer) != 0);

  const rx::u64 child_a = PackInChildHandle(0x100000001ull, 0x200000002ull);
  const rx::u64 child_a_again = PackInChildHandle(0x100000001ull, 0x200000002ull);
  const rx::u64 child_b = PackInChildHandle(0x100000003ull, 0x200000002ull);
  Check("pack-in child handles are deterministic", child_a == child_a_again);
  Check("pack-in parent participates in the handle", child_a != child_b);
  Check("pack-in handles use the synthetic plugin range", (child_a >> 32) >= 0x8000);

  // A remapped inert child has no script attachment of its own. Registering the
  // owner mapping must still let record-backed natives resolve the synthetic
  // handle to the child's source record.
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "recreation_prop_streamingtest";
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  rx::bethesda::GameProfile profile;
  profile.game = rx::bethesda::Game::kSkyrimSe;
  profile.name = "test";
  profile.plugin_version = 1.0f;
  rx::bethesda::PluginWriter plugin(profile);
  plugin.set_master(true);
  rx::bethesda::RecordBuilder source_record(FourCc('S', 'T', 'A', 'T'),
                                             rx::bethesda::RawFormId{0x00000800});
  source_record.EditorId("InertPackChild");
  plugin.AddRecord(source_record.record());
  const std::filesystem::path plugin_path = dir / "PackSource.esm";
  Check("writes synthetic pack source", plugin.Save(plugin_path.string()));

  rx::bethesda::LoadOrder order;
  order.Append("PackSource.esm");
  rx::bethesda::RecordStore records;
  Check("loads synthetic pack source", records.LoadAll(dir.string(), order, profile));
  const rx::bethesda::GlobalFormId source{0, 0x800};
  const rx::u64 owner = 0x0000000100000900ull;
  const rx::u64 runtime = PackInChildHandle(owner, source.packed());
  rx::script::skyrim::RecordBackedSkyrimBindings bindings(&records);
  bindings.SetRuntimeForm(owner, source.packed(), runtime);
  Check("inert synthetic child resolves record-backed natives",
        bindings.GetFormType(rx::script::papyrus::ObjectRef{runtime}) == 35);
  std::filesystem::remove_all(dir, ec);

  if (failures == 0) {
    std::puts("prop streaming: all checks passed");
    return 0;
  }
  std::printf("prop streaming: %d checks FAILED\n", failures);
  return 1;
}
