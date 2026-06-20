#include "content_domain.h"

#include <vector>

#include "bethesda/archive.h"
#include "bethesda/converters.h"
#include "bethesda/record.h"
#include "bethesda/script_attachment.h"
#include "core/log.h"
#include "quest/quest_def.h"
#include "script/papyrus/value.h"

#include <filesystem>

namespace rec {

bool ContentDomain::Load(bethesda::Game game, const std::string& data_dir,
                         const std::string& plugins_txt, bool replica_mode) {
  game_ = game != bethesda::Game::kUnknown ? game
                                           : bethesda::GameProfile::DetectFromDataDir(data_dir);
  if (game_ == bethesda::Game::kUnknown) {
    REC_ERROR("could not detect a supported game in {}", data_dir);
    return false;
  }
  data_dir_ = data_dir;
  profile_ = &bethesda::GameProfile::For(game_);
  REC_INFO("domain: loading {} from {}", profile_->name, data_dir);

  // Archives first, then loose files so they win over archives.
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(data_dir, ec)) {
    if (auto provider = bethesda::OpenArchive(entry.path().string())) vfs_.Mount(std::move(provider));
  }
  vfs_.Mount(asset::MakeLooseFileProvider(data_dir));

  assets_ = std::make_unique<asset::AssetDatabase>(vfs_);
  bethesda::RegisterConverters(*assets_, *profile_);

  auto order = bethesda::LoadOrder::FromPluginsTxt(plugins_txt, *profile_);
  if (!records_.LoadAll(data_dir, order, *profile_)) return false;
  REC_INFO("domain {}: {} plugins, {} records", profile_->name, order.plugins().size(),
           records_.record_count());

  for (const std::string& plugin : order.plugins())
    strings_.Load(vfs_, plugin, profile_->string_language);
  dialogue_.Build(records_);
  REC_INFO("domain {}: {} strings, {} dialogue topics", profile_->name, strings_.size(),
           dialogue_.topic_count());

  bindings_ = std::make_unique<script::skyrim::RecordBackedSkyrimBindings>(&records_);
  bindings_->set_strings(&strings_);
  bindings_->set_player(script::papyrus::ObjectRef{0x14});  // PlayerRef, shared by both games
  bindings_->set_replica_mode(replica_mode);

  scripts_ = std::make_unique<script::ScriptSystem>(game_, &vfs_, bindings_.get());
  // Hand the guest its VM so quest stage fragments can run on the guest thread.
  auto* binds = bindings_.get();
  scripts_->guest().Submit(
      [binds](script::papyrus::VirtualMachine& vm) { binds->set_vm(&vm); });
  return true;
}

int ContentDomain::AttachQuestScripts(int max_quests) {
  if (!scripts_) return 0;
  int quests = 0;
  int instances = 0;
  records_.EachOfType(
      FourCc('Q', 'U', 'S', 'T'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
        if (max_quests > 0 && quests >= max_quests) return;
        bethesda::Record record;
        if (!records_.Parse(id, &record)) return;
        const bethesda::Subrecord* vmad = record.Find(FourCc('V', 'M', 'A', 'D'));
        if (!vmad) return;
        bethesda::ScriptAttachment attachment;
        std::vector<bethesda::QuestStageFragment> fragments;
        if (!bethesda::ParseQuestFragments(vmad->data, &attachment, &fragments) ||
            attachment.scripts.empty())
          return;
        u64 handle = static_cast<u64>(id.plugin) << 32 | id.local_id;
        auto created = scripts_->AttachScripts(handle, attachment);
        if (created.empty()) return;
        ++quests;
        instances += static_cast<int>(created.size());
        quest::QuestDef def = quest::ParseQuestDefinition(handle, record, &strings_);
        auto* binds = bindings_.get();
        scripts_->guest().Submit(
            [binds, handle, def = std::move(def), fragments = std::move(fragments)](
                script::papyrus::VirtualMachine&) mutable {
              binds->quest_system().SetDefinition(std::move(def));
              for (const auto& f : fragments) binds->SetStageFragment(handle, f.stage, f.function);
            });
      });
  REC_INFO("domain {}: instantiated {} scripts across {} quests", profile_->name, instances,
           quests);
  return quests;
}

void ContentDomain::Tick(f32 dt) {
  if (scripts_) scripts_->Tick(dt);
}

}  // namespace rec
