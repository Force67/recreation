#include "face.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <base/option.h>

#include "asset/asset_database.h"
#include "asset/subdivide.h"
#include "bethesda/nif.h"
#include "core/log.h"
#include "engine_context.h"
#include "render/core/renderer.h"

namespace rec {

// Loop-subdivision levels applied to head-part meshes (0/1/2). A plain option,
// not a RenderSetting, so the engine.cc preset merge never clobbers it. Shared
// by the faces demo and the per-NPC actor head assembly.
static base::Option<int> HeadSubdiv{"head.subdiv", 1, "REC_HEAD_SUBDIV",
                                    "loop subdivision levels on facegen head parts"};

namespace {

// HDPT model paths are stored backslashed and without the meshes/ root.
std::string ModelPath(const std::string& model) {
  std::string path = asset::NormalizePath(model);
  if (!path.starts_with("meshes/")) path = "meshes/" + path;
  return path;
}

bool IsSkinPart(bethesda::HeadPartType t) {
  return t == bethesda::HeadPartType::kFace || t == bethesda::HeadPartType::kEyes ||
         t == bethesda::HeadPartType::kEyebrows || t == bethesda::HeadPartType::kFacialHair ||
         t == bethesda::HeadPartType::kScar;
}

void RecomputeBounds(asset::Mesh& mesh) {
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) return;
  f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const asset::Vertex& v : mesh.lods[0].vertices)
    for (int k = 0; k < 3; ++k) {
      lo[k] = std::min(lo[k], v.position[k]);
      hi[k] = std::max(hi[k], v.position[k]);
    }
  f32 r2 = 0;
  for (int k = 0; k < 3; ++k) {
    mesh.bounds_center[k] = (lo[k] + hi[k]) * 0.5f;
    f32 d = hi[k] - mesh.bounds_center[k];
    r2 += d * d;
  }
  mesh.bounds_radius = std::sqrt(r2);
}

}  // namespace

FaceBuilder::FaceBuilder(EngineContext& ctx) : ctx_(ctx) {}

const bethesda::TriMorphSet* FaceBuilder::Tri(const std::string& vfs_path) {
  std::string path = ModelPath(vfs_path);
  u64 key = asset::MakeAssetId(path).hash;
  if (auto* cached = tri_cache_.find(key))
    return (*cached)->vertex_count ? &**cached : nullptr;
  auto set = base::MakeUnique<bethesda::TriMorphSet>();  // 0 verts == absent
  if (auto bytes = ctx_.vfs->Read(path)) {
    if (auto parsed = bethesda::ParseTri(ByteSpan(bytes->data(), bytes->size())))
      *set = std::move(*parsed);
    else
      REC_WARN("face: tri parse failed {}", path);
  }
  bool ok = set->vertex_count != 0;
  auto* slot = tri_cache_.insert(key, std::move(set)).first;
  return ok ? &**slot : nullptr;
}

const asset::Mesh* FaceBuilder::BasePartMesh(const std::string& model_path) {
  std::string path = ModelPath(model_path);
  u64 key = asset::MakeAssetId(path).hash;
  if (auto* cached = mesh_cache_.find(key))
    return (*cached)->lods.empty() ? nullptr : &**cached;

  auto mesh = base::MakeUnique<asset::Mesh>();  // empty lods == absent
  if (auto bytes = ctx_.vfs->Read(path)) {
    bethesda::NifConversion conv =
        bethesda::ConvertNifRigid(ByteSpan(bytes->data(), bytes->size()),
                                  asset::MakeAssetId(path), path);
    if (conv.mesh && !conv.mesh->lods.empty() && !conv.mesh->lods[0].vertices.empty()) {
      for (const std::string& tex : conv.texture_paths)
        if (const asset::Texture* t = ctx_.assets->LoadTexture(tex)) ctx_.renderer->UploadTexture(*t);
      for (const asset::Material& m : conv.materials) {
        ctx_.assets->AddMaterial(m);
        ctx_.renderer->UploadMaterial(m);
      }
      *mesh = std::move(*conv.mesh);
    }
  } else {
    REC_WARN("face: head part mesh not found {}", path);
  }
  bool ok = !mesh->lods.empty();
  auto* slot = mesh_cache_.insert(key, std::move(mesh)).first;
  return ok ? &**slot : nullptr;
}

bool FaceBuilder::AssembleNpc(bethesda::GlobalFormId npc, FaceState* out) {
  auto face = bethesda::ResolveNpcFace(*ctx_.records, npc);
  if (!face) {
    REC_WARN("face: not an NPC_ {:04x}:{:06x}", npc.plugin, npc.local_id);
    return false;
  }
  auto race = bethesda::ResolveRaceHead(*ctx_.records, face->race);
  if (!race) {
    REC_WARN("face: no race head data for {}", face->editor_id);
    return false;
  }
  const bethesda::RaceSexHead& sex = face->female ? race->female : race->male;

  *out = FaceState{};
  out->builder_ = this;
  out->subdiv_levels_ = static_cast<u32>(std::clamp(HeadSubdiv.get(), 0, 2));
  out->female_ = face->female;
  out->race_morph_ = race->editor_id;  // race tri morphs are named by race EDID
  for (u32 i = 0; i < bethesda::kNam9Count; ++i)
    out->nam9_[i] = face->has_face_morph ? face->face_morph[i] : 0.0f;
  for (int i = 0; i < 4; ++i) out->nama_[i] = face->has_face_parts ? face->face_parts[i] : -1;
  if (face->has_skin_tone)
    for (int k = 0; k < 3; ++k) out->skin_tone_[k] = face->skin_tone[k];

  // The NPC's PNAM parts override the race defaults of the same type; every
  // race default of a type the NPC does not touch is kept.
  base::Vector<bethesda::HeadPart> npc_parts;
  bool overrides[7] = {};
  for (bethesda::GlobalFormId hp : face->head_parts) {
    if (auto part = bethesda::ResolveHeadPart(*ctx_.records, hp)) {
      u32 t = static_cast<u32>(part->type);
      if (t < 7) overrides[t] = true;
      npc_parts.push_back(std::move(*part));
    }
  }
  base::Vector<bethesda::HeadPart> merged;
  for (const bethesda::RaceHeadPart& rp : sex.parts) {
    if (rp.head_part.plugin == 0xffff) continue;
    if (auto part = bethesda::ResolveHeadPart(*ctx_.records, rp.head_part)) {
      u32 t = static_cast<u32>(part->type);
      if (t < 7 && overrides[t]) continue;
      merged.push_back(std::move(*part));
    }
  }
  for (bethesda::HeadPart& p : npc_parts) merged.push_back(std::move(p));

  const std::string npc_tag =
      std::to_string(npc.plugin) + "_" + std::to_string(npc.local_id);
  for (const bethesda::HeadPart& hp : merged) {
    if (hp.model.empty()) continue;
    const asset::Mesh* base = BasePartMesh(hp.model);
    if (!base) continue;
    FaceState::Part part;
    part.type = hp.type;
    part.base = base;
    part.subdivide = IsSkinPart(hp.type);
    part.label = hp.editor_id;
    // NAM0 marker 0 = race-blend tri, 2 = chargen tri (1 = base/expression).
    for (const bethesda::HeadPartTri& tri : hp.tris) {
      if (tri.type == 0) part.race_tri = Tri(tri.path);
      else if (tri.type == 2) part.chargen_tri = Tri(tri.path);
    }
    part.out_id = asset::MakeAssetId("facegen/" + npc_tag + "/" + hp.editor_id);
    out->parts_.push_back(std::move(part));
  }
  if (out->parts_.empty()) {
    REC_WARN("face: no renderable head parts for {}", face->editor_id);
    return false;
  }
  REC_INFO("face: assembled {} ({} parts, {}, race {})", face->editor_id,
           out->parts_.size(), face->female ? "female" : "male", race->editor_id);
  return true;
}

void FaceState::SetNam9(u32 index, f32 value) {
  if (index < bethesda::kNam9Count) nam9_[index] = value;
}

void FaceState::SetNama(u32 slot, i32 index) {
  if (slot < 4) nama_[slot] = index;
}

void FaceState::SetMorph(const std::string& chargen_morph, f32 weight) {
  for (bethesda::MorphWeight& w : extra_) {
    if (w.name == chargen_morph) {
      w.weight = weight;
      return;
    }
  }
  if (weight != 0.0f) extra_.push_back({chargen_morph, weight});
}

void FaceState::SetRaceBlend(const std::string& race_morph) { race_morph_ = race_morph; }

void FaceState::SetSubdivLevels(u32 levels) { subdiv_levels_ = std::min(levels, 3u); }

base::Vector<std::string> FaceState::ChargenMorphNames() const {
  base::Vector<std::string> names;
  for (const Part& p : parts_) {
    if (p.type != bethesda::HeadPartType::kFace || !p.chargen_tri) continue;
    for (const bethesda::TriMorph& m : p.chargen_tri->morphs) names.push_back(m.name);
    break;
  }
  return names;
}

f32 FaceState::RebuildAndUpload() {
  auto t0 = std::chrono::steady_clock::now();
  if (!builder_) return 0;
  render::Renderer& renderer = *builder_->ctx_.renderer;

  base::Vector<bethesda::MorphWeight> chargen;
  bethesda::CollectFaceMorphs(nam9_, nama_, &chargen);
  for (const bethesda::MorphWeight& w : extra_) chargen.push_back(w);

  built_.clear();
  for (Part& part : parts_) {
    asset::Mesh mesh = *part.base;  // copy the cached base (Bethesda object space)
    if (!mesh.lods.empty()) {
      bethesda::ApplyHeadMorphs(mesh.lods[0], part.race_tri, race_morph_, part.chargen_tri,
                                chargen);
      if (part.subdivide)
        asset::SubdivideLoop(mesh.lods[0], subdiv_levels_);
      else
        asset::RecomputeNormalsTangents(mesh.lods[0]);
    }
    mesh.id = part.out_id;
    RecomputeBounds(mesh);
    renderer.UploadMesh(mesh);
    built_.push_back({part.out_id, part.type, part.subdivide});
  }

  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<f32, std::milli>(t1 - t0).count();
}

}  // namespace rec
