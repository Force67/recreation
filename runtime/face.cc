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

namespace rx {

// Loop-subdivision levels applied to head-part meshes (0/1/2). A plain option,
// not a RenderSetting, so the engine.cc preset merge never clobbers it. Shared
// by the faces demo and the per-NPC actor head assembly.
static base::Option<int> HeadSubdiv{"head.subdiv", 1, "RX_HEAD_SUBDIV",
                                    "loop subdivision levels on facegen head parts"};

namespace {

// HDPT model paths are stored backslashed and without the meshes/ root.
std::string ModelPath(const std::string& model) {
  std::string path = asset::NormalizePath(model);
  if (!path.starts_with("meshes/")) path = "meshes/" + path;
  return path;
}

// TXST/NIF texture paths are stored backslashed without the textures/ root.
std::string TexturePath(const std::string& tex) {
  if (tex.empty()) return {};
  std::string path = asset::NormalizePath(tex);
  if (!path.starts_with("textures/")) path = "textures/" + path;
  return path;
}

// Bethesda head/face normals ship as model-space maps (_msn): the shader must
// take the object-space normal path, not the tangent-space TBN (which smears
// them into bruised streaks). Eye/brow tangent-space _n maps stay tangent.
bool IsModelSpaceNormal(const std::string& path) {
  size_t dot = path.rfind('.');
  std::string_view stem = dot == std::string::npos ? std::string_view{path}
                                                   : std::string_view{path}.substr(0, dot);
  return stem.ends_with("_msn");
}

// --- CPU BCn decode for the facetint bake (BC1/BC2/BC3/RGBA8 -> sRGB rgb8) ---
// Same block layout the land + hair bakes decode; kept local so the face bake
// stays self-contained.
void Rgb565(u16 c, u8 out[3]) {
  out[0] = static_cast<u8>(((c >> 11) & 0x1f) * 255 / 31);
  out[1] = static_cast<u8>(((c >> 5) & 0x3f) * 255 / 63);
  out[2] = static_cast<u8>((c & 0x1f) * 255 / 31);
}

void DecodeBc1Block(const u8* block, bool always_four, u8 out[16][3]) {
  u16 c0, c1;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  u8 pal[4][3];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  if (always_four || c0 > c1) {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<u8>((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = static_cast<u8>((pal[0][k] + 2 * pal[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      pal[2][k] = static_cast<u8>((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  u32 bits;
  std::memcpy(&bits, block + 4, 4);
  for (u32 i = 0; i < 16; ++i) std::memcpy(out[i], pal[(bits >> (i * 2)) & 3], 3);
}

size_t MipOffset(const asset::Texture& t, u32 mip, u32* w, u32* h) {
  bool compressed = t.format != asset::TextureFormat::kRgba8;
  size_t block = t.format == asset::TextureFormat::kBc1 ? 8 : 16;
  size_t offset = 0;
  for (u32 m = 0; m < mip; ++m) {
    u32 mw = std::max(1u, t.width >> m);
    u32 mh = std::max(1u, t.height >> m);
    offset += compressed ? ((mw + 3) / 4) * ((mh + 3) / 4) * block
                         : static_cast<size_t>(mw) * mh * 4;
  }
  *w = std::max(1u, t.width >> mip);
  *h = std::max(1u, t.height >> mip);
  return offset;
}

// Decodes the texture at the finest mip whose largest side is <= max_dim into a
// tight sRGB rgb8 grid. Returns false for unsupported formats (BC4/5/7).
bool DecodeToRgb8(const asset::Texture& t, u32 max_dim, u32* out_w, u32* out_h,
                  base::Vector<u8>* out) {
  if (t.format != asset::TextureFormat::kBc1 && t.format != asset::TextureFormat::kBc2 &&
      t.format != asset::TextureFormat::kBc3 && t.format != asset::TextureFormat::kRgba8) {
    return false;
  }
  u32 mip = 0;
  for (u32 m = 0; m + 1 < t.mip_count; ++m) {
    if (std::max(t.width >> (m + 1), t.height >> (m + 1)) < max_dim) break;
    mip = m + 1;
  }
  u32 w, h;
  size_t offset = MipOffset(t, mip, &w, &h);
  out->resize(static_cast<size_t>(w) * h * 3);
  if (t.format == asset::TextureFormat::kRgba8) {
    size_t need = static_cast<size_t>(w) * h * 4;
    if (offset + need > t.data.size()) return false;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
      std::memcpy(out->data() + i * 3, t.data.data() + offset + i * 4, 3);
  } else {
    bool alpha_block = t.format != asset::TextureFormat::kBc1;
    size_t block_size = alpha_block ? 16 : 8;
    u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (offset + static_cast<size_t>(bw) * bh * block_size > t.data.size()) return false;
    for (u32 by = 0; by < bh; ++by)
      for (u32 bx = 0; bx < bw; ++bx) {
        const u8* block =
            t.data.data() + offset + (static_cast<size_t>(by) * bw + bx) * block_size;
        u8 colors[16][3];
        DecodeBc1Block(block + (alpha_block ? 8 : 0), alpha_block, colors);
        for (u32 py = 0; py < 4; ++py)
          for (u32 px = 0; px < 4; ++px) {
            u32 x = bx * 4 + px, y = by * 4 + py;
            if (x >= w || y >= h) continue;
            std::memcpy(out->data() + (static_cast<size_t>(y) * w + x) * 3, colors[py * 4 + px], 3);
          }
      }
  }
  *out_w = w;
  *out_h = h;
  return true;
}

f32 Overlay(f32 b, f32 t) {
  return b < 0.5f ? 2.0f * b * t : 1.0f - 2.0f * (1.0f - b) * (1.0f - t);
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
      RX_WARN("face: tri parse failed {}", path);
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
      // Record the first material's diffuse/normal paths so the facetint bake can
      // reuse the racial skin as its base and keep the authored normal map.
      if (!conv.materials.empty() && !part_tex_.find(key)) {
        PartTextures pt;
        auto path_of = [&](asset::AssetId id) -> std::string {
          for (const std::string& tp : conv.texture_paths)
            if (asset::MakeAssetId(tp).hash == id.hash) return tp;
          return {};
        };
        pt.diffuse = path_of(conv.materials[0].base_color);
        pt.normal = path_of(conv.materials[0].normal);
        part_tex_.insert(key, std::move(pt));
      }
      *mesh = std::move(*conv.mesh);
    }
  } else {
    RX_WARN("face: head part mesh not found {}", path);
  }
  bool ok = !mesh->lods.empty();
  auto* slot = mesh_cache_.insert(key, std::move(mesh)).first;
  return ok ? &**slot : nullptr;
}

const FaceBuilder::PartTextures* FaceBuilder::Textures(const std::string& model_path) {
  u64 key = asset::MakeAssetId(ModelPath(model_path)).hash;
  return part_tex_.find(key);
}

const FaceBuilder::Decoded* FaceBuilder::DecodedTexture(const std::string& path) {
  u64 key = asset::MakeAssetId(path).hash;
  if (auto* cached = decoded_cache_.find(key)) return (*cached)->w ? &**cached : nullptr;
  auto dec = base::MakeUnique<Decoded>();  // w==0 == absent
  if (const asset::Texture* t = ctx_.assets->LoadTexture(path)) {
    DecodeToRgb8(*t, 512, &dec->w, &dec->h, &dec->rgb);
  }
  bool ok = dec->w != 0;
  auto* slot = decoded_cache_.insert(key, std::move(dec)).first;
  return ok ? &**slot : nullptr;
}

bool FaceBuilder::AssembleNpc(bethesda::GlobalFormId npc, FaceState* out) {
  auto face = bethesda::ResolveNpcFace(*ctx_.records, npc);
  if (!face) {
    RX_WARN("face: not an NPC_ {:04x}:{:06x}", npc.plugin, npc.local_id);
    return false;
  }
  auto race = bethesda::ResolveRaceHead(*ctx_.records, face->race);
  if (!race) {
    RX_WARN("face: no race head data for {}", face->editor_id);
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
    if (hp.type == bethesda::HeadPartType::kHair) out->hair_model_ = hp.model;
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
    part.model = hp.model;
    out->parts_.push_back(std::move(part));
  }
  if (out->parts_.empty()) {
    RX_WARN("face: no renderable head parts for {}", face->editor_id);
    return false;
  }

  // Facetint inputs. Base skin diffuse/normal: the face part's NIF (racial skin),
  // falling back to the race default face texture set (DFTM) when the NIF part
  // bound none. The NPC's FTST is the game's own pre-baked facetint, which we
  // replace with our own composite so a chargen slider can re-tint live.
  out->npc_tag_ = npc_tag;
  for (const bethesda::HeadPart& hp : merged) {
    if (hp.type != bethesda::HeadPartType::kFace || hp.model.empty()) continue;
    if (const PartTextures* pt = Textures(hp.model)) {
      out->face_diffuse_ = pt->diffuse;
      out->face_normal_ = pt->normal;
    }
    break;
  }
  if (out->face_diffuse_.empty()) {
    if (auto ts = bethesda::ResolveTextureSet(*ctx_.records, sex.default_face_texture_set)) {
      out->face_diffuse_ = TexturePath(ts->diffuse);
      if (out->face_normal_.empty()) out->face_normal_ = TexturePath(ts->normal);
    }
  }
  // Resolve each NPC tint layer to its race mask + colour. An absent mask means
  // full-face coverage (the base skin-tone layer).
  for (const bethesda::NpcTintLayer& nl : face->tint_layers) {
    if (nl.interpolation == 0) continue;
    const bethesda::RaceTintLayer* rl = nullptr;
    for (const bethesda::RaceTintLayer& cand : sex.tint_layers)
      if (cand.index == nl.index) { rl = &cand; break; }
    FaceState::TintLayer layer;
    if (rl) {
      layer.mask = TexturePath(rl->mask_texture);
      layer.type = rl->mask_type;
    }
    for (int k = 0; k < 3; ++k) layer.color[k] = nl.color[k] / 255.0f;
    layer.alpha = std::clamp(static_cast<f32>(nl.interpolation) / 100.0f, 0.0f, 1.0f);
    out->tint_layers_.push_back(std::move(layer));
  }
  out->tint_dirty_ = true;
  f32 bake_ms = out->BakeFaceTint();

  // Eyes, brows and beard get their own materials so they read right: eyes as a
  // wet glossy dielectric (the NIF authors them env-mapped, which renders as a
  // dark metal ball without a cubemap), brows/beard tinted to the hair colour.
  f32 hair_col[3] = {0.32f, 0.24f, 0.18f};
  if (auto clfm = bethesda::ResolveColorForm(*ctx_.records, face->hair_color))
    for (int k = 0; k < 3; ++k) hair_col[k] = clfm->rgba[k] / 255.0f;
  for (int k = 0; k < 3; ++k) out->hair_color_[k] = hair_col[k];
  for (FaceState::Part& p : out->parts_) {
    const PartTextures* pt = Textures(p.model);
    if (p.type == bethesda::HeadPartType::kEyes) {
      if (!pt || pt->diffuse.empty()) continue;
      asset::Material m;
      m.id = asset::MakeAssetId("facegen/eye/" + npc_tag + "/" + p.label);
      m.base_color = asset::MakeAssetId(pt->diffuse);
      if (!pt->normal.empty()) {
        m.normal = asset::MakeAssetId(pt->normal);
        m.normal_model_space = IsModelSpaceNormal(pt->normal);
      }
      m.metallic_factor = 0.0f;
      // Glossy, but not mirror-tight: a broader lobe keeps a catchlight on the eye
      // as the head turns to 3/4 (a near-mirror highlight slides off and the eye
      // reads dead), and a higher albedo lifts the sclera/iris out of the socket.
      m.roughness_factor = 0.18f;
      for (int k = 0; k < 3; ++k) m.base_color_factor[k] = 1.5f;
      ctx_.assets->AddMaterial(m);
      ctx_.renderer->UploadMaterial(m);
      p.material_override = m.id;
    } else if (p.type == bethesda::HeadPartType::kEyebrows ||
               p.type == bethesda::HeadPartType::kFacialHair) {
      asset::Material m;
      m.id = asset::MakeAssetId("facegen/brow/" + npc_tag + "/" + p.label);
      if (pt && !pt->diffuse.empty()) m.base_color = asset::MakeAssetId(pt->diffuse);
      if (pt && !pt->normal.empty()) {
        m.normal = asset::MakeAssetId(pt->normal);
        m.normal_model_space = IsModelSpaceNormal(pt->normal);
      }
      for (int k = 0; k < 3; ++k) m.base_color_factor[k] = hair_col[k];
      m.roughness_factor = 0.6f;
      m.alpha_mode = asset::AlphaMode::kMask;
      m.alpha_cutoff = 0.25f;
      m.two_sided = true;
      m.hair = true;  // kajiya-kay strand response along the card tangent
      ctx_.assets->AddMaterial(m);
      ctx_.renderer->UploadMaterial(m);
      p.material_override = m.id;
    }
  }

  RX_INFO("face: assembled {} ({} parts, {}, race {}, {} tint layers, facetint bake {:.2f} ms)",
           face->editor_id, out->parts_.size(), face->female ? "female" : "male", race->editor_id,
           out->tint_layers_.size(), bake_ms);
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

void FaceState::SetSkinTone(f32 r, f32 g, f32 b) {
  skin_tone_[0] = r;
  skin_tone_[1] = g;
  skin_tone_[2] = b;
  // Re-tint the skin-tone layer too (it, not the QNAM fallback multiply, colours
  // the face when the NPC authored one) so the slider actually moves skin colour.
  for (TintLayer& tl : tint_layers_)
    if (tl.type == 6) {
      tl.color[0] = r;
      tl.color[1] = g;
      tl.color[2] = b;
    }
  tint_dirty_ = true;
}

f32 FaceState::BakeFaceTint() {
  if (!builder_ || face_diffuse_.empty() || !tint_dirty_) return 0;
  auto t0 = std::chrono::steady_clock::now();
  render::Renderer& renderer = *builder_->ctx_.renderer;

  const FaceBuilder::Decoded* base = builder_->DecodedTexture(face_diffuse_);
  if (!base || base->w == 0) return 0;
  const u32 w = base->w, h = base->h;

  // Pre-decode the tint masks (cached in the builder; shared across a race). The
  // type-6 skin-tone layer paints the whole face with the racial skin colour (its
  // TINC equals QNAM); the rest are localized detail (eye sockets, cheeks, lips,
  // chin, warpaint, dirt).
  struct Layer {
    const FaceBuilder::Decoded* mask;  // null = full-face coverage
    f32 color[3];
    f32 alpha;
  };
  base::Vector<Layer> layers;
  bool has_skin_layer = false;
  for (const TintLayer& tl : tint_layers_) {
    Layer l;
    l.mask = tl.mask.empty() ? nullptr : builder_->DecodedTexture(tl.mask);
    // A layer that names a mask we could not decode must be dropped, not smeared
    // across the whole face (its localized colour would crush the albedo).
    if (!tl.mask.empty() && !l.mask) continue;
    for (int k = 0; k < 3; ++k) l.color[k] = tl.color[k];
    l.alpha = tl.alpha;
    if (tl.type == 6) has_skin_layer = true;
    layers.push_back(l);
  }

  // Version the ids so a live re-bake actually replaces the GPU texture/material
  // (UploadTexture / UploadMaterial early-out on a known id).
  const std::string ver = std::to_string(tint_version_);
  face_texture_ = asset::MakeAssetId("facetint/tex/" + npc_tag_ + "/" + ver);
  face_material_ = asset::MakeAssetId("facetint/mat/" + npc_tag_ + "/" + ver);

  asset::Texture tex;
  tex.id = face_texture_;
  tex.format = asset::TextureFormat::kRgba8;
  tex.width = w;
  tex.height = h;
  tex.mip_count = 1;
  tex.is_srgb = true;
  tex.data.resize(static_cast<size_t>(w) * h * 4);

  // Composite in sRGB (matching the CK's gamma-space facetint bake). The tint
  // layers overlay the skin colour and detail through their masks; when an NPC
  // authored no skin-tone layer, fall back to a QNAM multiply so the face still
  // gets its racial colour instead of the flat grey base.
  for (u32 y = 0; y < h; ++y) {
    for (u32 x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      f32 c[3];
      for (int k = 0; k < 3; ++k) {
        c[k] = base->rgb[i * 3 + k] / 255.0f;
        if (!has_skin_layer) c[k] *= skin_tone_[k];
        c[k] = std::clamp(c[k], 0.0f, 1.0f);
      }
      for (const Layer& l : layers) {
        f32 cov = 1.0f;
        if (l.mask) {
          u32 mx = x * l.mask->w / w, my = y * l.mask->h / h;
          const u8* m = &l.mask->rgb[(static_cast<size_t>(my) * l.mask->w + mx) * 3];
          cov = (m[0] + m[1] + m[2]) / (3.0f * 255.0f);
        }
        f32 a = cov * l.alpha;
        if (a <= 0.0f) continue;
        for (int k = 0; k < 3; ++k) c[k] = c[k] + (Overlay(c[k], l.color[k]) - c[k]) * a;
      }
      u8* dst = &tex.data[i * 4];
      for (int k = 0; k < 3; ++k) dst[k] = static_cast<u8>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f);
      dst[3] = 255;
    }
  }
  renderer.UploadTexture(tex);

  asset::Material m;
  m.id = face_material_;
  m.base_color = face_texture_;
  if (!face_normal_.empty()) {
    m.normal = asset::MakeAssetId(face_normal_);
    m.normal_model_space = IsModelSpaceNormal(face_normal_);
  }
  m.metallic_factor = 0.0f;
  m.roughness_factor = 0.55f;  // skin
  m.skin = true;
  m.subsurface = 0.16f;  // gentle analytic transmit; screen-space sss does the rest
  // Physically based skin scattering: reddish multiple-scatter colour with red
  // reaching furthest under the surface (3-layer skin dmfp). Face skin is a bit
  // thinner than the generic default. The blood-flow system drives perfusion.
  m.skin_params.scatter_color[0] = 0.87f;
  m.skin_params.scatter_color[1] = 0.57f;
  m.skin_params.scatter_color[2] = 0.43f;
  m.skin_params.mfp[0] = 0.9f;   // mm, red
  m.skin_params.mfp[1] = 0.32f;  // green
  m.skin_params.mfp[2] = 0.18f;  // blue
  m.skin_params.perfusion = 0.5f;
  builder_->ctx_.assets->AddMaterial(m);
  renderer.UploadMaterial(m);

  for (Part& p : parts_)
    if (p.type == bethesda::HeadPartType::kFace) p.material_override = face_material_;

  ++tint_version_;
  tint_dirty_ = false;
  return std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

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
    if (part.material_override) {
      for (asset::MeshLod& lod : mesh.lods)
        for (asset::Submesh& sm : lod.submeshes) sm.material = part.material_override;
    }
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
    if (part.type == bethesda::HeadPartType::kFace) {
      for (int k = 0; k < 3; ++k) head_center_[k] = mesh.bounds_center[k];
      head_radius_ = mesh.bounds_radius;
    }
    renderer.UploadMesh(mesh);
    built_.push_back({part.out_id, part.type, part.subdivide});
  }

  auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<f32, std::milli>(t1 - t0).count();
}

}  // namespace rx
