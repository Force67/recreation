#include "recreation/script/papyrus_vm.h"

#include "recreation/core/log.h"

namespace rec::script {

PapyrusVm::PapyrusVm(bethesda::Game game) : game_(game) { BindNatives(); }

bool PapyrusVm::LoadScript(ByteSpan pex_data) {
  // TODO: pex header (magic 0xFA57C0DE, big endian for SSE, little for
  // FO4/76), string table, object/state/function tables.
  REC_WARN("pex loading not implemented");
  return false;
}

void PapyrusVm::BindNatives() {
  switch (game_) {
    case bethesda::Game::kSkyrimSe:
      // TODO: Skyrim native set (Game, Form, ObjectReference, Actor, ...).
      break;
    case bethesda::Game::kFallout4:
    case bethesda::Game::kFallout76:
      // TODO: Fallout native set.
      break;
    case bethesda::Game::kUnknown:
      break;
  }
}

void PapyrusVm::Tick(ecs::World& world, f32 dt) {
  // TODO: resume suspended stacks, dispatch queued events (OnInit, OnUpdate,
  // OnActivate) raised by gameplay systems.
}

}  // namespace rec::script
