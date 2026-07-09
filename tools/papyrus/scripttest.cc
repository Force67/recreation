// scripttest: full plugin-to-VM integration against real assets.
//
//   scripttest <data_dir>
//
// Finds a scripted record, reads its VMAD, then drives ScriptSystem to load the
// attached script (and its ancestor chain) out of the BSA, instantiate it on
// the form, seed its properties from the editor-baked values, and raise OnInit.
// Verifies the chain loaded and a property round-trips.

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>

#include "asset/vfs.h"
#include "bethesda/archive.h"
#include "bethesda/load_order.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "script/papyrus/vm.h"
#include "script/script_system.h"

namespace {

using namespace rx;
using namespace rx::bethesda;
using rx::script::ScriptSystem;
using rx::script::papyrus::ObjectRef;
using rx::script::papyrus::VirtualMachine;

struct Found {
  GlobalFormId id;
  ScriptAttachment attachment;
};

// First record of `type` carrying at least one script with a property.
std::optional<Found> FindScripted(RecordStore& records, u32 type) {
  std::optional<Found> result;
  records.EachOfType(type, [&](GlobalFormId id, const RecordStore::StoredRecord&) {
    if (result) return;
    Record rec;
    if (!records.Parse(id, &rec)) return;
    const Subrecord* vmad = rec.Find(FourCc('V', 'M', 'A', 'D'));
    if (!vmad) return;
    ScriptAttachment att;
    if (!ParseScriptAttachment(vmad->data, &att)) return;
    for (const ScriptEntry& s : att.scripts)
      if (!s.properties.empty()) {
        result = Found{id, std::move(att)};
        return;
      }
  });
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <data_dir>\n", argv[0]);
    return 2;
  }
  std::string data_dir = argv[1];

  asset::Vfs vfs;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec))
    if (auto p = bethesda::OpenArchive(entry.path().string())) vfs.Mount(std::move(p));

  const auto& profile = GameProfile::For(GameProfile::DetectFromDataDir(data_dir));
  auto order = LoadOrder::FromPluginsTxt(data_dir + "/../plugins.txt", profile);
  RecordStore records;
  if (!records.LoadAll(data_dir, order, profile)) {
    std::printf("failed to load records\n");
    return 1;
  }

  std::optional<Found> found = FindScripted(records, FourCc('Q', 'U', 'S', 'T'));
  if (!found) {
    std::printf("no scripted quest found\n");
    return 1;
  }
  const ScriptEntry& first = [&]() -> const ScriptEntry& {
    for (const ScriptEntry& s : found->attachment.scripts)
      if (!s.properties.empty()) return s;
    return found->attachment.scripts.front();
  }();
  rx::u64 handle = static_cast<rx::u64>(found->id.plugin) << 32 | found->id.local_id;
  std::printf("form %04x:%06x -> script %s (%zu properties)\n", found->id.plugin,
              found->id.local_id, first.name.c_str(), first.properties.size());

  ScriptSystem system(bethesda::Game::kSkyrimSe, &vfs, nullptr);
  auto handles = system.AttachScripts(handle, found->attachment);

  int failures = 0;
  auto check = [&](const char* what, bool ok) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
  };

  check("attached at least one instance", !handles.empty());
  check("loaded script + ancestor chain (>1 type)", system.loaded_script_count() > 1);
  bool quest_loaded =
      system.guest().SubmitFor([](VirtualMachine& vm) { return vm.HasScript("Quest"); }).get();
  check("ancestor Quest.pex loaded from BSA", quest_loaded);

  // Round-trip the first property through the live instance.
  const ScriptProperty& prop = first.properties.front();
  ObjectRef inst = ObjectRef{handle};
  auto value = system.guest()
                   .SubmitFor([inst, name = prop.name](VirtualMachine& vm) {
                     return vm.GetProperty(inst, name);
                   })
                   .get();
  bool round_trip = false;
  if (prop.type == 1)
    round_trip = value.as_object().handle == prop.object_value.form_id;
  else if (prop.type == 3)
    round_trip = value.ToInt() == prop.int_value;
  else if (prop.type == 4)
    round_trip = value.ToFloat() == prop.float_value;
  else if (prop.type == 2)
    round_trip = value.ToString() == prop.string_value;
  else if (prop.type == 5)
    round_trip = value.ToBool() == prop.bool_value;
  else
    round_trip = true;  // array property not seeded; not asserted
  std::printf("  property %s seeded and read back: %s\n", prop.name.c_str(),
              round_trip ? "ok" : "FAIL");
  if (!round_trip) ++failures;

  std::printf("%s (%d failures)\n", failures ? "SCRIPTTEST FAILED" : "SCRIPTTEST PASSED", failures);
  return failures ? 1 : 0;
}
