#include "chargen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <base/option.h>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "asset/primitives.h"
#include "asset/skeleton.h"
#include "bethesda/converters.h"
#include "bethesda/nif.h"
#include "bethesda/record.h"
#include "chargen_layout.h"
#include "core/input.h"
#include "core/log.h"
#include "core/math.h"
#include "ecs/world.h"
#include "engine_context.h"
#include "render/core/renderer.h"
#include "render/geometry/hair_groom.h"
#include "world/components.h"

namespace rx {
namespace {

// Config knobs, populated from the environment by base::InitOptionsFromEnv().
base::Option<const char*> ChargenOut{"chargen.out", nullptr, "RX_CHARGEN_OUT",
                                     "where the character-creation preset is saved/loaded"};
base::Option<const char*> ChargenScript{"chargen.script", nullptr, "RX_CHARGEN_SCRIPT",
                                        "headless chargen edits applied after boot (e.g. nam9:3=0.8,page:1)"};

constexpr f32 kStatusSeconds = 4.0f;
constexpr f32 kOrbitSens = 0.006f;   // radians per cursor pixel
constexpr f32 kUnitsToMeters = 0.01428f;

// Portrait framing: the head sits at eye height in a studio orbit. The camera
// pivots around the head, defaults to a slight 3/4 angle, and zooms between a
// face close-up and a bust; the look target sits a little below the eyes so
// they land near the upper-third line.
constexpr f32 kOrbitDistDefault = 0.56f;
constexpr f32 kOrbitDistMin = 0.36f;  // face close-up (near plane is 0.1)
constexpr f32 kOrbitDistMax = 0.80f;  // bust, never further out
constexpr f32 kOrbitYawDefault = 0.35f;  // slight 3/4
constexpr f32 kOrbitYawMax = 0.6f;       // studio orbit; keeps the backdrop behind
constexpr f32 kOrbitPitchMax = 0.45f;
constexpr f32 kLookDrop = 0.05f;  // look below the pivot -> eyes at the upper third

// "NoseUp" -> "Nose Up": a space before an uppercase that follows a lowercase.
std::string Spacify(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i > 0 && s[i] >= 'A' && s[i] <= 'Z' && s[i - 1] >= 'a' && s[i - 1] <= 'z') out += ' ';
    out += s[i];
  }
  return out;
}

}  // namespace

CharGen::CharGen(EngineContext& ctx) : ctx_(ctx) {}
CharGen::~CharGen() {
  if (groom_ && ctx_.renderer) ctx_.renderer->DestroyHairGroom(groom_);
}

void CharGen::Enter() {
  if (active_) return;
  if (!ctx_.records || !ctx_.renderer || !ctx_.vfs || !ctx_.assets) {
    RX_WARN("chargen: services not ready");
    return;
  }
  BuildRaces();
  if (races_.empty()) {
    RX_WARN("chargen: no playable human-head races resolved; not entering");
    return;
  }
  race_ = 0;
  for (size_t i = 0; i < races_.size(); ++i)
    if (races_[i].edid == "NordRace") race_ = static_cast<int>(i);

  builder_ = std::make_unique<FaceBuilder>(ctx_);
  save_path_ = ChargenOut.get() ? ChargenOut.get() : "chargen_preset.json";

  const bool loaded = Load();
  Reassemble(!loaded);  // a saved preset's edits win over the assembled preset
  SetupSceneAndCamera();

  active_ = true;
  ctx_.walk_mode = false;
  RX_INFO("chargen: entered ({} races, preview {} {}, {} presets)", races_.size(),
           races_[race_].display, sex_ ? "female" : "male", preset_count_);
  PushView();
}

void CharGen::BuildRaces() {
  races_.clear();
  static const struct {
    const char* edid;
    const char* disp;
  } kWant[] = {
      {"NordRace", "Nord"},       {"ImperialRace", "Imperial"}, {"BretonRace", "Breton"},
      {"RedguardRace", "Redguard"}, {"DarkElfRace", "Dark Elf"}, {"HighElfRace", "High Elf"},
      {"WoodElfRace", "Wood Elf"}, {"OrcRace", "Orc"},
  };
  std::unordered_map<std::string, bethesda::GlobalFormId> found;
  const u32 kEdid = FourCc('E', 'D', 'I', 'D');
  ctx_.records->EachOfType(FourCc('R', 'A', 'C', 'E'),
                           [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
    bethesda::Record r;
    if (!ctx_.records->Parse(id, &r)) return;
    found[r.GetString(kEdid)] = id;
  });
  for (const auto& w : kWant) {
    auto it = found.find(w.edid);
    if (it != found.end()) races_.push_back({it->second, w.edid, w.disp});
  }
}

void CharGen::BuildHairCatalog() {
  hair_styles_.clear();
  const u8 sexbit = sex_ == 1 ? 0x04 : 0x02;
  ctx_.records->EachOfType(FourCc('H', 'D', 'P', 'T'),
                           [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord&) {
    if (hair_styles_.size() >= 24) return;
    auto hp = bethesda::ResolveHeadPart(*ctx_.records, id);
    if (!hp || hp->type != bethesda::HeadPartType::kHair || hp->model.empty()) return;
    if (!(hp->flags & 0x01)) return;  // playable
    if (hp->flags & 0x10) return;     // chained "extra part", not a base style
    const bool male = hp->flags & 0x02, female = hp->flags & 0x04;
    if ((male || female) && !(hp->flags & sexbit)) return;  // wrong sex
    hair_styles_.push_back({hp->model, hp->editor_id});
  });
}

void CharGen::ScanChargenMorphs() {
  morph_names_.clear();
  curated_morphs_.clear();
  nose_types_ = {0};
  eyes_types_ = {0};
  mouth_types_ = {0};
  auto typeIdx = [](const std::string& n, const char* pre, std::vector<int>& out) {
    const size_t pl = std::strlen(pre);
    if (n.size() > pl && n.compare(0, pl, pre) == 0) {
      int v = std::atoi(n.c_str() + pl);
      if (v > 0) out.push_back(v);
    }
  };
  std::unordered_set<std::string> present;
  for (const std::string& n : face_.ChargenMorphNames()) {
    morph_names_.push_back(n);
    present.insert(n);
    typeIdx(n, "NoseType", nose_types_);
    typeIdx(n, "EyesType", eyes_types_);
    typeIdx(n, "LipType", mouth_types_);
  }
  for (std::vector<int>* v : {&nose_types_, &eyes_types_, &mouth_types_}) {
    std::sort(v->begin(), v->end());
    v->erase(std::unique(v->begin(), v->end()), v->end());
  }
  // A curated set of directional morphs (distinct from the NAM9 sliders), kept to
  // those the head's chargen tri actually ships.
  static const char* kCur[] = {"BrowUp",       "BrowDown",   "JawWide",   "JawNarrow",
                               "CheekUp",      "CheekDown",  "NoseLong",  "NoseShort",
                               "LipMoveOut",   "LipMoveIn",  "EyesMoveUp", "EyesMoveDown",
                               "ChinMoveUp",   "ChinMoveDown"};
  for (const char* c : kCur)
    if (present.count(c)) curated_morphs_.push_back(c);
  // Fallback: if none of the curated names exist, expose the first handful of
  // non-type morphs so the advanced page still has sliders.
  if (curated_morphs_.empty()) {
    for (const std::string& n : morph_names_) {
      if (n.rfind("NoseType", 0) == 0 || n.rfind("EyesType", 0) == 0 || n.rfind("LipType", 0) == 0)
        continue;
      curated_morphs_.push_back(n);
      if (curated_morphs_.size() >= 10) break;
    }
  }
}

void CharGen::PushEditsToFace() {
  for (u32 i = 0; i < bethesda::kNam9Count; ++i) face_.SetNam9(i, nam9_[i]);
  for (int i = 0; i < 4; ++i) face_.SetNama(i, nama_[i]);
  face_.SetSkinTone(skin_[0], skin_[1], skin_[2]);
  for (const auto& m : morphs_) face_.SetMorph(m.first, m.second);
}

void CharGen::Reassemble(bool read_preset) {
  if (races_.empty() || !builder_) return;
  race_ = std::clamp(race_, 0, static_cast<int>(races_.size()) - 1);
  auto race = bethesda::ResolveRaceHead(*ctx_.records, races_[race_].form);
  if (!race) {
    RX_WARN("chargen: no race head data for {}", races_[race_].edid);
    return;
  }
  const bethesda::RaceSexHead& sh = sex_ == 1 ? race->female : race->male;
  preset_count_ = static_cast<int>(sh.presets.size());

  bool ok = false;
  if (preset_count_ > 0) {
    preset_ = ((preset_ % preset_count_) + preset_count_) % preset_count_;
    for (int t = 0; t < preset_count_ && !ok; ++t) {
      const int pi = (preset_ + t) % preset_count_;
      if (sh.presets[pi].plugin == 0xffff) continue;
      FaceState tmp;
      if (builder_->AssembleNpc(sh.presets[pi], &tmp)) {
        face_ = std::move(tmp);
        preset_ = pi;
        ok = true;
      }
    }
  }
  if (!ok) {
    RX_WARN("chargen: no assemblable preset for {} {}", races_[race_].edid,
             sex_ ? "female" : "male");
    if (face_.parts().empty()) return;  // first boot with nothing: bail
  }

  face_.SetRaceBlend(races_[race_].edid);
  ScanChargenMorphs();
  if (read_preset) {
    for (u32 i = 0; i < bethesda::kNam9Count; ++i) nam9_[i] = face_.nam9()[i];
    for (int i = 0; i < 4; ++i) nama_[i] = face_.nama()[i];
    for (int k = 0; k < 3; ++k) skin_[k] = face_.skin_tone()[k];
    morphs_.clear();
  }
  PushEditsToFace();
  face_.BakeFaceTint();
  face_.RebuildAndUpload();
  SpawnHeadEntities();

  // Hair colours from this race/sex head (AHCM), styles from the HDPT catalog.
  hair_colors_.clear();
  for (bethesda::GlobalFormId cf : sh.hair_colors) {
    auto clfm = bethesda::ResolveColorForm(*ctx_.records, cf);
    if (!clfm) continue;
    hair_colors_.push_back({clfm->editor_id,
                            {clfm->rgba[0] / 255.0f, clfm->rgba[1] / 255.0f, clfm->rgba[2] / 255.0f}});
    if (hair_colors_.size() >= 16) break;
  }
  BuildHairCatalog();
  if (read_preset) {
    hair_style_ = 0;
    for (size_t i = 0; i < hair_styles_.size(); ++i)
      if (asset::NormalizePath(hair_styles_[i].model) == asset::NormalizePath(face_.hair_model())) {
        hair_style_ = static_cast<int>(i);
        break;
      }
    hair_color_ = -1;
  } else {
    if (!hair_styles_.empty())
      hair_style_ = std::clamp(hair_style_, 0, static_cast<int>(hair_styles_.size()) - 1);
    if (hair_color_ >= static_cast<int>(hair_colors_.size())) hair_color_ = -1;
  }
  RebuildHairGroom();
}

void CharGen::SpawnHeadEntities() {
  for (ecs::Entity e : head_entities_)
    if (ctx_.world->IsAlive(e)) ctx_.world->Destroy(e);
  head_entities_.clear();
  const Quat basis = QuatFromAxisAngle({1, 0, 0}, -1.57079633f);
  for (const BuiltFacePart& part : face_.parts()) {
    if (part.type == bethesda::HeadPartType::kHair) continue;  // groom replaces the card
    ecs::Entity e = ctx_.world->Create();
    ctx_.world->Add(e, world::Transform{.position = {0, 0, 0},
                                        .rotation = {basis.x, basis.y, basis.z, basis.w},
                                        .scale = kUnitsToMeters});
    ctx_.world->Add(e, world::Renderable{part.mesh});
    head_entities_.push_back(e);
  }
}

// Hair HDPT NIFs are authored head-bone-local (an actor rides them on the head
// bone; the standalone heads have no skeleton), so the groom needs the head
// bone's rest world position as a fixed translation. Read it once from the real
// skeleton and convert to engine space; the fallback constant is that same
// offset measured against Skyrim SE's skeleton.nif.
Vec3 CharGen::HeadBoneOffset() {
  static bool resolved = false;
  static Vec3 offset{0.0f, 1.66f, -0.04f};  // Skyrim SE fallback
  if (resolved) return offset;
  resolved = true;
  const char* kSkeleton = "meshes/actors/character/character assets/skeleton.nif";
  auto bytes = ctx_.vfs->Read(kSkeleton);
  if (!bytes) return offset;
  asset::Skeleton skel;
  if (!bethesda::ConvertNifSkeleton(ByteSpan(bytes->data(), bytes->size()),
                                    asset::MakeAssetId(kSkeleton), &skel)) {
    return offset;
  }
  const i32 head = skel.Find("NPC Head [Head]");
  if (head < 0) return offset;
  // Compose the parent-relative rest transforms up the chain (Bethesda object
  // space), then map to engine space like static NIF geometry.
  Vec3 t{0, 0, 0};
  Quat r{0, 0, 0, 1};
  f32 s = 1.0f;
  base::Vector<i32> chain;
  for (i32 b = head; b >= 0; b = skel.bones[b].parent) chain.push_back(b);
  for (size_t i = chain.size(); i-- > 0;) {
    const asset::Bone& bone = skel.bones[chain[i]];
    t = t + Rotate(r, bone.bind_translation * s);
    r = r * bone.bind_rotation;
    s *= bone.bind_scale;
  }
  offset = Vec3{t.x, t.z, -t.y} * kUnitsToMeters;
  RX_INFO("chargen: head bone rest at ({:.3f}, {:.3f}, {:.3f})", offset.x, offset.y, offset.z);
  return offset;
}

void CharGen::RebuildHairGroom() {
  render::Renderer& r = *ctx_.renderer;
  if (groom_) {
    r.DestroyHairGroom(groom_);
    groom_ = 0;
  }
  const std::string model =
      !hair_styles_.empty() ? hair_styles_[hair_style_].model : face_.hair_model();
  if (model.empty()) return;
  std::string path = asset::NormalizePath(model);
  if (!path.starts_with("meshes/")) path = "meshes/" + path;
  auto bytes = ctx_.vfs->Read(path);
  if (!bytes) return;
  bethesda::NifConversion conv = bethesda::ConvertNifRigid(
      ByteSpan(bytes->data(), bytes->size()), asset::MakeAssetId(path), path);
  if (!conv.mesh || conv.mesh->lods.empty() || conv.mesh->lods[0].vertices.empty()) return;
  const asset::Texture* diffuse = nullptr;
  for (const std::string& tp : conv.texture_paths) {
    diffuse = ctx_.assets->LoadTexture(tp);
    if (diffuse) break;
  }
  Vec3 tint = (hair_color_ >= 0 && hair_color_ < static_cast<int>(hair_colors_.size()))
                  ? hair_colors_[hair_color_].second
                  : Vec3{face_.hair_color()[0], face_.hair_color()[1], face_.hair_color()[2]};
  render::GroomParams p;
  p.recenter = false;  // keep authored head-bone-local coords; translate onto the head
  p.tint = {tint.x, tint.y, tint.z};
  p.diffuse = diffuse;
  p.guide_count = 7000;
  p.children_per_guide = 24;
  p.strand_width = 0.0013f;
  p.clump_radius = 0.007f;  // wider clumps fill the gaps between guides (no scalp show-through)
  p.frizz = 0.4f;  // tidy portrait flyaway; the curl still de-slabs into locks
  groom_ = r.CreateHairGroom(*conv.mesh, p, MakeTranslation(HeadBoneOffset()));
}

void CharGen::SetupSceneAndCamera() {
  if (scene_ready_) return;
  scene_ready_ = true;
  // A soft portrait key over a raised ambient fill, matching the faces demo: the
  // head reads without the harsh midday sun blowing out the skin. scene_owns_sun
  // stops the day/night cycle re-driving these every frame.
  ctx_.scene_owns_sun = true;
  auto& s = ctx_.renderer->settings();
  s.sun_direction = {-0.3f, -0.5f, 0.8f};
  s.sun_intensity = 2.0f;
  s.sun_color = {1.0f, 0.97f, 0.94f};
  s.ambient = 0.32f;
  s.dof = false;
  // No cast shadows: a floating head has no contact shadow to lose, the soft
  // portrait key plus the baked facegen tint carry the form, and it drops the
  // head's own sun shadow that otherwise specks the studio backdrop below it.
  s.rt_shadows = false;
  // Fixed exposure: the dark backdrop dominates the frame, so auto exposure
  // would crank up and wash out both the backdrop and the skin.
  s.auto_exposure = false;
  s.exposure = 0.95f;

  // A big dark backdrop behind the head (the head faces -Z; the clamped orbit
  // keeps the camera on that side), so the portrait reads against a studio
  // gradient instead of the empty sky/sea horizon.
  asset::Material back;
  back.id = asset::MakeAssetId("chargen/backdrop/material");
  back.base_color_factor[0] = 0.02f;
  back.base_color_factor[1] = 0.022f;
  back.base_color_factor[2] = 0.028f;
  back.roughness_factor = 0.92f;
  ctx_.renderer->UploadMaterial(back);
  // Far enough back that the head's sun shadow falls below the framed view (10 m
  // at this sun angle), big enough that the clamped orbit never sees past its
  // edges even at the frame corners.
  asset::Mesh cube = asset::MakeCube(60.0f, asset::MakeAssetId("chargen/backdrop"));
  for (asset::MeshLod& lod : cube.lods) {
    for (asset::Submesh& sm : lod.submeshes) sm.material = back.id;
    if (lod.submeshes.empty())
      lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), back.id});
  }
  ctx_.renderer->UploadMesh(cube);
  ecs::Entity e = ctx_.world->Create();
  ctx_.world->Add(e, world::Transform{.position = {0.0f, 0.0f, 70.0f}});
  ctx_.world->Add(e, world::Renderable{cube.id});

  orbit_yaw_ = kOrbitYawDefault;
  orbit_pitch_ = 0.02f;
  orbit_dist_ = kOrbitDistDefault;
  UpdateCameraPose();
}

void CharGen::UpdateCameraPose() {
  const f32 cp = std::cos(orbit_pitch_), sp = std::sin(orbit_pitch_);
  const Vec3 off{std::sin(orbit_yaw_) * cp, sp, -std::cos(orbit_yaw_) * cp};
  const Vec3 eye = head_center_ + off * orbit_dist_;
  ctx_.camera->set_position(eye);
  // Look a touch below the pivot so the eyes land near the upper-third line.
  const Vec3 d = Normalize(head_center_ - Vec3{0, kLookDrop, 0} - eye);
  ctx_.camera->set_yaw_pitch(std::atan2(d.x, -d.z), std::asin(std::clamp(d.y, -1.0f, 1.0f)));
}

std::vector<CharGen::Control> CharGen::BuildControls() const {
  std::vector<Control> c;
  if (page_ == 0) {
    for (u32 i = 0; i < bethesda::kNam9Count; ++i)
      c.push_back({Control::Kind::kNam9, bethesda::Nam9SliderInfo(i).label, "",
                   static_cast<int>(i), -1.0f, 1.0f});
  } else if (page_ == 1) {
    c.push_back({Control::Kind::kNamaType, "Nose Type", "", 0, 0, 0});
    c.push_back({Control::Kind::kNamaType, "Eyes Type", "", 2, 0, 0});
    c.push_back({Control::Kind::kNamaType, "Mouth Type", "", 3, 0, 0});
    for (const std::string& m : curated_morphs_)
      c.push_back({Control::Kind::kMorph, Spacify(m), m, 0, 0.0f, 1.5f});
  } else {
    c.push_back({Control::Kind::kSkin, "Skin Red", "", 0, 0.0f, 1.0f});
    c.push_back({Control::Kind::kSkin, "Skin Green", "", 1, 0.0f, 1.0f});
    c.push_back({Control::Kind::kSkin, "Skin Blue", "", 2, 0.0f, 1.0f});
    c.push_back({Control::Kind::kHairStyle, "Hair Style", "", 0, 0, 0});
    c.push_back({Control::Kind::kHairColor, "Hair Colour", "", 0, 0, 0});
  }
  return c;
}

namespace {
// Position of an index in a sorted type list (0 = base), for cyclers.
int TypePos(const std::vector<int>& list, int value) {
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == value) return static_cast<int>(i);
  return 0;
}
f32 MorphWeightOf(const std::vector<std::pair<std::string, f32>>& v, const std::string& name) {
  for (const auto& m : v)
    if (m.first == name) return m.second;
  return 0.0f;
}
}  // namespace

void CharGen::ApplyControl(const Control& cd, f32 v01) {
  v01 = std::clamp(v01, 0.0f, 1.0f);
  const f32 value = cd.mn + v01 * (cd.mx - cd.mn);
  switch (cd.kind) {
    case Control::Kind::kNam9:
      nam9_[cd.slot] = value;
      face_.SetNam9(cd.slot, value);
      face_.RebuildAndUpload();
      break;
    case Control::Kind::kMorph: {
      bool found = false;
      for (auto& m : morphs_)
        if (m.first == cd.name) {
          m.second = value;
          found = true;
        }
      if (!found) morphs_.push_back({cd.name, value});
      face_.SetMorph(cd.name, value);
      face_.RebuildAndUpload();
      break;
    }
    case Control::Kind::kSkin:
      skin_[cd.slot] = value;
      face_.SetSkinTone(skin_[0], skin_[1], skin_[2]);  // re-bake deferred to release
      break;
    default:
      break;  // cyclers do not drag
  }
}

void CharGen::CycleControl(const Control& cd, int dir) {
  switch (cd.kind) {
    case Control::Kind::kNamaType: {
      const std::vector<int>& list =
          cd.slot == 0 ? nose_types_ : cd.slot == 2 ? eyes_types_ : mouth_types_;
      if (list.empty()) break;
      int pos = TypePos(list, nama_[cd.slot]);
      pos = ((pos + dir) % static_cast<int>(list.size()) + static_cast<int>(list.size())) %
            static_cast<int>(list.size());
      nama_[cd.slot] = list[pos];
      face_.SetNama(cd.slot, nama_[cd.slot]);
      face_.RebuildAndUpload();
      break;
    }
    case Control::Kind::kHairStyle: {
      if (hair_styles_.empty()) break;
      const int n = static_cast<int>(hair_styles_.size());
      hair_style_ = ((hair_style_ + dir) % n + n) % n;
      RebuildHairGroom();
      break;
    }
    case Control::Kind::kHairColor: {
      if (hair_colors_.empty()) break;
      const int n = static_cast<int>(hair_colors_.size());
      int pos = hair_color_ < 0 ? 0 : hair_color_;
      hair_color_ = ((pos + dir) % n + n) % n;
      if (groom_) ctx_.renderer->SetHairGroomTint(groom_, hair_colors_[hair_color_].second);
      break;
    }
    default:
      break;
  }
}

void CharGen::SelectRace(int i) {
  if (i == race_ || i < 0 || i >= static_cast<int>(races_.size())) return;
  race_ = i;
  preset_ = 0;
  Reassemble(true);
  status_ = "Race: " + races_[race_].display;
  status_age_ = 0;
}

void CharGen::SelectSex(int sex) {
  if (sex == sex_) return;
  sex_ = sex;
  preset_ = 0;
  Reassemble(true);
  status_ = sex_ ? "Female" : "Male";
  status_age_ = 0;
}

void CharGen::StepPreset(int dir) {
  if (preset_count_ <= 0) return;
  preset_ = ((preset_ + dir) % preset_count_ + preset_count_) % preset_count_;
  Reassemble(true);
}

void CharGen::Reset() {
  Reassemble(true);
  status_ = "Reset to preset";
  status_age_ = 0;
}

void CharGen::Randomize() {
  static u32 seed = 0x1234567u;
  auto rnd = [&]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<f32>(seed >> 8) / 16777216.0f;
  };
  auto pick = [&](const std::vector<int>& v) {
    return v.empty() ? 0 : v[static_cast<size_t>(rnd() * v.size()) % v.size()];
  };
  for (u32 i = 0; i < bethesda::kNam9Count; ++i) nam9_[i] = (rnd() * 2.0f - 1.0f) * 0.5f;
  if (nose_types_.size() > 1) nama_[0] = pick(nose_types_);
  if (eyes_types_.size() > 1) nama_[2] = pick(eyes_types_);
  if (mouth_types_.size() > 1) nama_[3] = pick(mouth_types_);
  skin_[0] = 0.45f + rnd() * 0.4f;
  skin_[1] = 0.32f + rnd() * 0.35f;
  skin_[2] = 0.26f + rnd() * 0.3f;
  morphs_.clear();
  for (const std::string& m : curated_morphs_)
    if (rnd() < 0.4f) morphs_.push_back({m, rnd() * 0.9f});
  if (!hair_styles_.empty())
    hair_style_ = static_cast<int>(static_cast<size_t>(rnd() * hair_styles_.size()) % hair_styles_.size());
  if (!hair_colors_.empty())
    hair_color_ = static_cast<int>(static_cast<size_t>(rnd() * hair_colors_.size()) % hair_colors_.size());
  PushEditsToFace();
  face_.BakeFaceTint();
  face_.RebuildAndUpload();
  SpawnHeadEntities();
  RebuildHairGroom();
  status_ = "Randomized";
  status_age_ = 0;
}

void CharGen::Save() {
  chargen::CharGenPreset p;
  p.race = races_.empty() ? "NordRace" : races_[race_].edid;
  p.sex = sex_;
  p.preset = preset_;
  p.subdiv = static_cast<int>(face_.subdiv_levels());
  p.hair_style = hair_styles_.empty() ? -1 : hair_style_;
  p.hair_color = hair_color_;
  const Vec3 hc = (hair_color_ >= 0 && hair_color_ < static_cast<int>(hair_colors_.size()))
                      ? hair_colors_[hair_color_].second
                      : Vec3{face_.hair_color()[0], face_.hair_color()[1], face_.hair_color()[2]};
  for (int k = 0; k < 3; ++k) p.skin[k] = skin_[k];
  p.hair[0] = hc.x;
  p.hair[1] = hc.y;
  p.hair[2] = hc.z;
  for (int k = 0; k < 4; ++k) p.nama[k] = nama_[k];
  for (u32 i = 0; i < bethesda::kNam9Count; ++i)
    if (std::fabs(nam9_[i]) > 1e-4f) p.nam9.push_back({static_cast<int>(i), nam9_[i]});
  for (const auto& m : morphs_)
    if (std::fabs(m.second) > 1e-4f) p.morphs.push_back(m);

  std::ofstream out(save_path_, std::ios::trunc);
  if (!out) {
    status_ = "Save failed: " + save_path_;
    status_age_ = 0;
    RX_WARN("chargen: cannot open {} for writing", save_path_);
    return;
  }
  out << chargen::SerializeCharGenPreset(p);
  status_ = "Saved to " + save_path_;
  status_age_ = 0;
  RX_INFO("chargen: saved preset to {}", save_path_);
}

bool CharGen::Load() {
  std::ifstream in(save_path_);
  if (!in) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  chargen::CharGenPreset p;
  if (!chargen::ParseCharGenPreset(ss.str(), &p)) return false;
  for (size_t i = 0; i < races_.size(); ++i)
    if (races_[i].edid == p.race) race_ = static_cast<int>(i);
  sex_ = p.sex ? 1 : 0;
  preset_ = p.preset < 0 ? 0 : p.preset;
  for (int k = 0; k < 3; ++k) skin_[k] = p.skin[k];
  for (int k = 0; k < 4; ++k) nama_[k] = p.nama[k];
  for (u32 i = 0; i < bethesda::kNam9Count; ++i) nam9_[i] = 0;
  for (const auto& [i, v] : p.nam9)
    if (i >= 0 && i < static_cast<int>(bethesda::kNam9Count)) nam9_[i] = v;
  morphs_.clear();
  for (const auto& m : p.morphs) morphs_.push_back(m);
  hair_style_ = p.hair_style < 0 ? 0 : p.hair_style;
  hair_color_ = p.hair_color;
  RX_INFO("chargen: loaded preset from {}", save_path_);
  return true;
}

void CharGen::ApplyScript(const std::string& script) {
  auto valuePart = [](const std::string& s, char sep, std::string* rest) -> std::string {
    const size_t p = s.find(sep);
    if (p == std::string::npos) {
      *rest = "";
      return s;
    }
    *rest = s.substr(p + 1);
    return s.substr(0, p);
  };
  std::stringstream ts(script);
  std::string tok;
  bool edited = false;
  while (std::getline(ts, tok, ',')) {
    if (tok.empty()) continue;
    std::string rest;
    const std::string key = valuePart(tok, ':', &rest);
    if (key == "page") {
      page_ = std::clamp(std::atoi(rest.c_str()), 0, 2);
      row_first_ = 0;
    } else if (key == "sex") {
      SelectSex(std::atoi(rest.c_str()) ? 1 : 0);
    } else if (key == "race") {
      for (size_t i = 0; i < races_.size(); ++i)
        if (races_[i].edid == rest) SelectRace(static_cast<int>(i));
    } else if (key == "preset") {
      preset_ = std::atoi(rest.c_str());
      Reassemble(true);
    } else if (key == "randomize") {
      Randomize();
    } else if (key == "nam9") {
      std::string v;
      const int i = std::atoi(valuePart(rest, '=', &v).c_str());
      if (i >= 0 && i < static_cast<int>(bethesda::kNam9Count)) {
        nam9_[i] = static_cast<f32>(std::atof(v.c_str()));
        edited = true;
      }
    } else if (key == "morph") {
      std::string v;
      const std::string name = valuePart(rest, '=', &v);
      const f32 w = static_cast<f32>(std::atof(v.c_str()));
      bool found = false;
      for (auto& m : morphs_)
        if (m.first == name) {
          m.second = w;
          found = true;
        }
      if (!found) morphs_.push_back({name, w});
      edited = true;
    } else if (key == "nama" || key == "nose" || key == "eyes" || key == "mouth") {
      int slot = key == "nose" ? 0 : key == "eyes" ? 2 : key == "mouth" ? 3 : -1;
      int idx;
      if (slot < 0) {  // nama:S=I
        std::string v;
        slot = std::atoi(valuePart(rest, '=', &v).c_str());
        idx = std::atoi(v.c_str());
      } else {
        idx = std::atoi(rest.c_str());
      }
      if (slot >= 0 && slot < 4) {
        nama_[slot] = idx;
        edited = true;
      }
    } else if (key == "skin") {
      std::string g, b;
      const f32 r = static_cast<f32>(std::atof(valuePart(rest, ':', &g).c_str()));
      const f32 gg = static_cast<f32>(std::atof(valuePart(g, ':', &b).c_str()));
      skin_[0] = r;
      skin_[1] = gg;
      skin_[2] = static_cast<f32>(std::atof(b.c_str()));
      edited = true;
    } else if (key == "hair") {
      if (!hair_styles_.empty())
        hair_style_ = std::clamp(std::atoi(rest.c_str()), 0, static_cast<int>(hair_styles_.size()) - 1);
    } else if (key == "haircolor") {
      if (!hair_colors_.empty())
        hair_color_ = std::clamp(std::atoi(rest.c_str()), 0, static_cast<int>(hair_colors_.size()) - 1);
    }
  }
  // Reflect the accumulated edits once.
  PushEditsToFace();
  face_.BakeFaceTint();
  face_.RebuildAndUpload();
  SpawnHeadEntities();
  RebuildHairGroom();
  status_ = "Applied RX_CHARGEN_SCRIPT";
  status_age_ = 0;
  (void)edited;
  RX_INFO("chargen: applied script '{}'", script);
}

void CharGen::Update(const InputState& input, f32 dt) {
  if (!active_) return;
  elapsed_ += dt;
  status_age_ += dt;

  // Headless proof of interactivity: apply the scripted edits once, a few frames
  // in (after the first head + hair have uploaded).
  if (!script_done_ && elapsed_ > 0.4f) {
    script_done_ = true;
    if (const char* s = ChargenScript.get()) ApplyScript(s);
  }

  const f32 w = static_cast<f32>(ctx_.renderer->output_width());
  const f32 h = static_cast<f32>(ctx_.renderer->output_height());
  const f32 mx = input.mouse_x, my = input.mouse_y;
  const f32 dx = mx - prev_mx_, dy = my - prev_my_;
  const bool lmb = input.button(MouseButton::kLeft);
  const bool press = lmb && !prev_lmb_;
  const bool release = !lmb && prev_lmb_;

  const float rightLeft = w - kCgRightW;
  const bool inLeft = mx < kCgLeftW && my > kCgTop && my < h - kCgTop;
  const bool inRight = mx > rightLeft && my > kCgTop && my < h - kCgTop;
  const float contentW = kCgLeftW - 2 * kCgPad;
  const float sexW = (contentW - 8.0f) / 2.0f;
  const float tabW = (contentW - 2 * 6.0f) / 3.0f;

  std::vector<Control> controls = BuildControls();
  const int total = static_cast<int>(controls.size());
  const int max_first = std::max(0, total - kCgSliderRows);
  row_first_ = std::clamp(row_first_, 0, max_first);

  if (press) {
    if (inLeft) {
      auto hit = [&](float x0, float y0, float ww, float hh) {
        return mx >= x0 && mx <= x0 + ww && my >= y0 && my <= y0 + hh;
      };
      bool handled = false;
      for (int i = 0; i < static_cast<int>(races_.size()) && !handled; ++i)
        if (hit(kCgPad, kCgTop + kCgRaceY0 + i * kCgRaceRowH, contentW, kCgRaceRowH - 3.0f)) {
          SelectRace(i);
          handled = true;
        }
      if (!handled && hit(kCgPad, kCgTop + kCgSexY, sexW, kCgBtnH)) { SelectSex(0); handled = true; }
      if (!handled && hit(kCgPad + sexW + 8.0f, kCgTop + kCgSexY, sexW, kCgBtnH)) { SelectSex(1); handled = true; }
      if (!handled && hit(kCgPad, kCgTop + kCgPresetY, 40.0f, kCgBtnH)) { StepPreset(-1); handled = true; }
      if (!handled && hit(kCgPad + contentW - 40.0f, kCgTop + kCgPresetY, 40.0f, kCgBtnH)) { StepPreset(1); handled = true; }
      for (int i = 0; i < 3 && !handled; ++i)
        if (hit(kCgPad + i * (tabW + 6.0f), kCgTop + kCgPageY, tabW, 32.0f)) {
          page_ = i;
          row_first_ = 0;
          handled = true;
        }
      if (!handled && hit(kCgPad, kCgTop + kCgActY + 0 * kCgActH, contentW, 30.0f)) { Randomize(); handled = true; }
      if (!handled && hit(kCgPad, kCgTop + kCgActY + 1 * kCgActH, contentW, 30.0f)) { Reset(); handled = true; }
      if (!handled && hit(kCgPad, kCgTop + kCgActY + 2 * kCgActH, contentW, 30.0f)) { Save(); handled = true; }
    } else if (inRight) {
      auto hit = [&](float x0, float y0, float ww, float hh) {
        return mx >= x0 && mx <= x0 + ww && my >= y0 && my <= y0 + hh;
      };
      if (hit(w - 82.0f, kCgTop + 14.0f, 30.0f, 26.0f)) {
        row_first_ = std::max(0, row_first_ - 1);
      } else if (hit(w - 46.0f, kCgTop + 14.0f, 30.0f, 26.0f)) {
        row_first_ = std::min(max_first, row_first_ + 1);
      } else {
        for (int i = 0; i < kCgSliderRows; ++i) {
          const int ci = row_first_ + i;
          if (ci >= total) break;
          const float y0 = kCgTop + kCgRowsY0 + i * kCgRowH;
          if (my < y0 || my > y0 + kCgRowH - 6.0f) continue;
          if (mx < rightLeft + 8.0f || mx > w - 8.0f) continue;
          const Control& c = controls[ci];
          const float trackX = rightLeft + kCgTrackX;
          if (c.kind == Control::Kind::kNam9 || c.kind == Control::Kind::kMorph ||
              c.kind == Control::Kind::kSkin) {
            dragging_ = true;
            drag_control_ = ci;
            drag_skin_ = c.kind == Control::Kind::kSkin;
            ApplyControl(c, (mx - trackX) / kCgTrackW);
          } else {
            CycleControl(c, mx < trackX + kCgTrackW * 0.5f ? -1 : 1);
          }
          break;
        }
      }
    } else {
      orbiting_ = true;
    }
  }

  if (lmb) {
    if (dragging_ && drag_control_ >= 0 && drag_control_ < total) {
      const float trackX = rightLeft + kCgTrackX;
      ApplyControl(controls[drag_control_], (mx - trackX) / kCgTrackW);
    } else if (orbiting_) {
      orbit_yaw_ = std::clamp(orbit_yaw_ + dx * kOrbitSens, -kOrbitYawMax, kOrbitYawMax);
      orbit_pitch_ =
          std::clamp(orbit_pitch_ - dy * kOrbitSens, -kOrbitPitchMax, kOrbitPitchMax);
    }
  }

  if (release) {
    if (drag_skin_) face_.BakeFaceTint();  // re-tint on release (the bake leaks GPU ids)
    dragging_ = false;
    drag_control_ = -1;
    drag_skin_ = false;
    orbiting_ = false;
  }

  if (input.wheel != 0.0f) {
    if (inRight)
      row_first_ = std::clamp(row_first_ - (input.wheel > 0 ? 1 : -1), 0, max_first);
    else if (!inLeft)
      orbit_dist_ = std::clamp(orbit_dist_ - input.wheel * 0.06f, kOrbitDistMin, kOrbitDistMax);
  }

  UpdateCameraPose();
  prev_lmb_ = lmb;
  prev_mx_ = mx;
  prev_my_ = my;
  PushView();
}

void CharGen::PushView() {
  if (!ctx_.game_ui) return;
  CharGenView v;
  v.active = true;
  for (const Race& r : races_) v.races.push_back(r.display);
  v.race = race_;
  v.sex = sex_;
  v.page = page_;
  {
    char b[64];
    if (preset_count_ > 0)
      std::snprintf(b, sizeof(b), "Preset %d / %d", preset_ + 1, preset_count_);
    else
      std::snprintf(b, sizeof(b), "Preset -");
    v.preset_label = b;
  }
  v.status = status_age_ < kStatusSeconds ? status_ : "";
  const char* titles[3] = {"FACE", "ADVANCED", "APPEARANCE"};
  v.page_title = titles[std::clamp(page_, 0, 2)];

  std::vector<Control> controls = BuildControls();
  v.row_total = static_cast<int>(controls.size());
  const int max_first = std::max(0, v.row_total - kCgSliderRows);
  v.row_first = std::clamp(row_first_, 0, max_first);

  auto packRgb = [](const Vec3& c) -> u32 {
    auto ch = [](f32 v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (ch(c.x) << 24) | (ch(c.y) << 16) | (ch(c.z) << 8) | 0xffu;
  };

  for (int i = 0; i < kCgSliderRows; ++i) {
    const int ci = v.row_first + i;
    if (ci >= v.row_total) break;
    const Control& c = controls[ci];
    CharGenView::Row row;
    row.label = c.label;
    char b[48];
    switch (c.kind) {
      case Control::Kind::kNam9: {
        const f32 val = nam9_[c.slot];
        row.fill = (val + 1.0f) * 0.5f;
        std::snprintf(b, sizeof(b), "%+.2f", val);
        row.value = b;
        break;
      }
      case Control::Kind::kMorph: {
        const f32 val = MorphWeightOf(morphs_, c.name);
        row.fill = c.mx > 0 ? val / c.mx : 0.0f;
        std::snprintf(b, sizeof(b), "%.2f", val);
        row.value = b;
        break;
      }
      case Control::Kind::kSkin: {
        const f32 val = skin_[c.slot];
        row.fill = val;
        std::snprintf(b, sizeof(b), "%.2f", val);
        row.value = b;
        break;
      }
      case Control::Kind::kNamaType: {
        const std::vector<int>& list =
            c.slot == 0 ? nose_types_ : c.slot == 2 ? eyes_types_ : mouth_types_;
        const int pos = TypePos(list, nama_[c.slot]);
        row.kind = 1;
        row.fill = list.size() > 1 ? static_cast<f32>(pos) / (list.size() - 1) : 0.0f;
        row.value = nama_[c.slot] <= 0 ? std::string("Base") : std::to_string(nama_[c.slot]);
        break;
      }
      case Control::Kind::kHairStyle: {
        row.kind = 1;
        if (hair_styles_.empty()) {
          row.value = "-";
        } else {
          row.fill = hair_styles_.size() > 1
                         ? static_cast<f32>(hair_style_) / (hair_styles_.size() - 1)
                         : 0.0f;
          row.value = std::to_string(hair_style_ + 1) + " / " + std::to_string(hair_styles_.size());
        }
        break;
      }
      case Control::Kind::kHairColor: {
        row.kind = 1;
        if (hair_colors_.empty()) {
          row.value = "-";
        } else {
          const int idx = hair_color_ < 0 ? 0 : hair_color_;
          row.fill = hair_colors_.size() > 1
                         ? static_cast<f32>(idx) / (hair_colors_.size() - 1)
                         : 0.0f;
          row.value = std::to_string(idx + 1) + " / " + std::to_string(hair_colors_.size());
          row.swatch = packRgb(hair_colors_[idx].second);
        }
        break;
      }
    }
    v.rows.push_back(std::move(row));
  }
  ctx_.game_ui->SetCharGenView(v);
}

}  // namespace rx
