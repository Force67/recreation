#include "runtime/app/content_domain.h"

#include <base/containers/vector.h>
#include <base/memory/move.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/xstring.h>

#include <filesystem>

#include "components/bethesda/archive.h"
#include "components/bethesda/converters.h"
#include "components/bethesda/record.h"
#include "components/bethesda/script_attachment.h"
#include "components/quest/quest_def.h"
#include "components/script/papyrus/value.h"
#include "core/log.h"

namespace rx {

bool ContentDomain::Load(bethesda::Game game,
                         const base::String& data_dir,
                         const base::String& plugins_txt,
                         bool replica_mode) {
  game_ =
      game != bethesda::Game::kUnknown ? game : bethesda::GameProfile::DetectFromDataDir(data_dir);
  if (game_ == bethesda::Game::kUnknown) {
    RX_ERROR("could not detect a supported game in {}", data_dir);
    return false;
  }
  data_dir_ = data_dir;
  profile_ = &bethesda::GameProfile::For(game_);
  RX_INFO("domain: loading {} from {}", profile_->name, data_dir);

  // Archives first, then loose files so they win over archives.
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir.c_str(), ec)) {
    if (auto provider = bethesda::OpenArchive(entry.path().string()))
      vfs_.Mount(base::move(provider));
  }
  vfs_.Mount(asset::MakeLooseFileProvider(data_dir.c_str()));

  assets_ = base::MakeUnique<asset::AssetDatabase>(vfs_);
  bethesda::RegisterConverters(*assets_, *profile_);

  auto order = bethesda::LoadOrder::FromPluginsTxt(plugins_txt, *profile_);
  if (!records_.LoadAll(data_dir, order, *profile_))
    return false;
  RX_INFO("domain {}: {} plugins, {} records", profile_->name, order.plugins().size(),
          records_.record_count());

  for (const base::String& plugin : order.plugins())
    strings_.Load(vfs_, plugin, profile_->string_language);
  dialogue_.Build(records_);
  RX_INFO("domain {}: {} strings, {} dialogue topics", profile_->name, strings_.size(),
          dialogue_.topic_count());

  bindings_ = base::MakeUnique<script::skyrim::RecordBackedSkyrimBindings>(&records_);
  bindings_->set_strings(&strings_);
  bindings_->set_player(script::papyrus::ObjectRef{0x14});  // PlayerRef, shared by both games
  bindings_->set_replica_mode(replica_mode);

  scripts_ =
      base::MakeUnique<script::ScriptSystem>(game_, &vfs_, (bindings_ ? &*bindings_ : nullptr));
  // Hand the guest its VM so quest stage fragments can run on the guest thread.
  auto* binds = (bindings_ ? &*bindings_ : nullptr);
  scripts_->guest().Submit([binds](script::papyrus::VirtualMachine& vm) { binds->set_vm(&vm); });
  return true;
}

int ContentDomain::AttachQuestScripts(int max_quests) {
  if (!scripts_)
    return 0;
  int quests = 0;
  int instances = 0;
  records_.EachOfType(
      FourCc('Q', 'U', 'S', 'T'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        if (max_quests > 0 && quests >= max_quests)
          return;
        bethesda::Record record;
        if (!records_.Parse(id, &record))
          return;
        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
        if (!vmad)
          return;
        bethesda::ScriptAttachment attachment;
        base::Vector<bethesda::QuestStageFragment> fragments;
        if (!bethesda::ParseQuestFragments(vmad->data, &attachment, &fragments) ||
            attachment.scripts.empty())
          return;
        bethesda::ResolveScriptObjectForms(&attachment, [&](u32 raw) {
          return records_.ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin).packed();
        });
        u64 handle = static_cast<u64>(id.plugin) << 32 | id.local_id;
        // Best effort: a quest registers even when its Papyrus fails to load
        // (Starfield PEX is not executed yet) so its stage machine still runs
        // under the domain's microvm and replicates over the network.
        auto created = scripts_->AttachScripts(handle, attachment);
        ++quests;
        instances += static_cast<int>(created.size());
        quest::QuestDef def = quest::ParseQuestDefinition(handle, record, &strings_);
        auto* binds = (bindings_ ? &*bindings_ : nullptr);
        scripts_->guest().Submit(
            [binds, handle, def = base::move(def),
             fragments = base::move(fragments)](script::papyrus::VirtualMachine&) mutable {
              binds->quest_system().SetDefinition(base::move(def));
              for (const auto& f : fragments)
                binds->SetStageFragment(handle, f.stage, f.log_entry, f.function, {});
            });
      });
  RX_INFO("domain {}: instantiated {} scripts across {} quests", profile_->name, instances, quests);
  return quests;
}

void ContentDomain::Tick(f32 dt) {
  if (scripts_)
    scripts_->Tick(dt);
}

}  // namespace rx
