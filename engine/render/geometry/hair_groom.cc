#include "render/geometry/hair_groom.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/containers/unordered_map.h>

#include "core/log.h"

namespace rec::render {
namespace {

// Bethesda object space (Z-up, game units) -> engine space (Y-up, metres). Same
// mapping the cell streamer uses: engine = (x, z, -y) * units_to_metres.
constexpr f32 kUnitsToMeters = 0.01428f;

Vec3 ToEngine(const f32 p[3]) {
  return {p[0] * kUnitsToMeters, p[2] * kUnitsToMeters, -p[1] * kUnitsToMeters};
}

struct GVert {
  Vec3 p;
  f32 u = 0, v = 0;
};

struct Tri {
  u32 a, b, c;
};

// --- CPU diffuse sampling (BC1/BC3/RGBA8) --------------------------------

struct DecodedTex {
  u32 w = 0, h = 0;
  base::Vector<f32> rgba;  // linear rgb + [0,1] alpha, row major, 4 per texel
};

// BC4-style 8-byte alpha block (BC3's alpha half): two endpoints + 3-bit indices.
void DecodeBc4Alpha(const u8* block, f32 out[16]) {
  u32 a0 = block[0], a1 = block[1];
  u64 bits = 0;
  for (int i = 0; i < 6; ++i) bits |= static_cast<u64>(block[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) {
    u32 idx = static_cast<u32>((bits >> (3 * i)) & 0x7);
    f32 a;
    if (idx == 0) a = static_cast<f32>(a0);
    else if (idx == 1) a = static_cast<f32>(a1);
    else if (a0 > a1) a = ((8 - idx) * a0 + (idx - 1) * a1) / 7.0f;
    else if (idx < 6) a = ((6 - idx) * a0 + (idx - 1) * a1) / 5.0f;
    else a = (idx == 6) ? 0.0f : 255.0f;
    out[i] = a / 255.0f;
  }
}

void DecodeBc1Colors(const u8* block, bool bc3, u8 out[16][3]) {
  u16 c0, c1;
  std::memcpy(&c0, block, 2);
  std::memcpy(&c1, block + 2, 2);
  auto expand = [](u16 c, u8 rgb[3]) {
    rgb[0] = static_cast<u8>(((c >> 11) & 0x1f) * 255 / 31);
    rgb[1] = static_cast<u8>(((c >> 5) & 0x3f) * 255 / 63);
    rgb[2] = static_cast<u8>((c & 0x1f) * 255 / 31);
  };
  u8 pal[4][3];
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  bool four = bc3 || c0 > c1;
  for (int k = 0; k < 3; ++k) {
    if (four) {
      pal[2][k] = static_cast<u8>((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = static_cast<u8>((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = static_cast<u8>((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  u32 bits;
  std::memcpy(&bits, block + 4, 4);
  for (int i = 0; i < 16; ++i) {
    u32 idx = (bits >> (i * 2)) & 0x3;
    for (int k = 0; k < 3; ++k) out[i][k] = pal[idx][k];
  }
}

size_t MipOffset(const asset::Texture& t, u32 mip, u32* w, u32* h) {
  bool compressed = t.format != asset::TextureFormat::kRgba8;
  size_t block = (t.format == asset::TextureFormat::kBc1) ? 8 : 16;
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

bool DecodeDiffuse(const asset::Texture& t, u32 target_max, DecodedTex* out) {
  if (t.format != asset::TextureFormat::kBc1 && t.format != asset::TextureFormat::kBc3 &&
      t.format != asset::TextureFormat::kRgba8) {
    return false;
  }
  u32 mip = 0;
  for (u32 m = 0; m + 1 < t.mip_count; ++m) {
    if (std::max(t.width >> m, t.height >> m) <= target_max) break;
    mip = m + 1;
  }
  u32 w, h;
  size_t offset = MipOffset(t, mip, &w, &h);
  out->w = w;
  out->h = h;
  out->rgba.resize(static_cast<size_t>(w) * h * 4);
  auto to_lin = [&](u8 c) {
    f32 x = c / 255.0f;
    return t.is_srgb ? std::pow(x, 2.2f) : x;
  };
  if (t.format == asset::TextureFormat::kRgba8) {
    if (offset + static_cast<size_t>(w) * h * 4 > t.data.size()) return false;
    for (u32 i = 0; i < w * h; ++i) {
      for (int k = 0; k < 3; ++k) out->rgba[i * 4 + k] = to_lin(t.data[offset + i * 4 + k]);
      out->rgba[i * 4 + 3] = t.data[offset + i * 4 + 3] / 255.0f;
    }
    return true;
  }
  bool bc3 = t.format == asset::TextureFormat::kBc3;
  size_t block_size = bc3 ? 16 : 8;
  u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
  if (offset + static_cast<size_t>(bw) * bh * block_size > t.data.size()) return false;
  for (u32 by = 0; by < bh; ++by) {
    for (u32 bx = 0; bx < bw; ++bx) {
      const u8* blk = t.data.data() + offset + (static_cast<size_t>(by) * bw + bx) * block_size;
      u8 colors[16][3];
      DecodeBc1Colors(blk + (bc3 ? 8 : 0), bc3, colors);
      f32 alpha[16];
      if (bc3) {
        DecodeBc4Alpha(blk, alpha);
      } else {
        // DXT1 1-bit alpha: the punch-through palette index (only in the 3-colour
        // mode) is transparent; opaque cards decode to 1.
        u16 c0, c1;
        std::memcpy(&c0, blk, 2);
        std::memcpy(&c1, blk + 2, 2);
        u32 bits;
        std::memcpy(&bits, blk + 4, 4);
        bool four = c0 > c1;
        for (int i = 0; i < 16; ++i) {
          u32 idx = (bits >> (i * 2)) & 0x3;
          alpha[i] = (!four && idx == 3) ? 0.0f : 1.0f;
        }
      }
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 4; ++px) {
          u32 x = bx * 4 + px, y = by * 4 + py;
          if (x >= w || y >= h) continue;
          const u8* c = colors[py * 4 + px];
          f32* dst = &out->rgba[(static_cast<size_t>(y) * w + x) * 4];
          for (int k = 0; k < 3; ++k) dst[k] = to_lin(c[k]);
          dst[3] = alpha[py * 4 + px];
        }
      }
    }
  }
  return true;
}

struct Texel {
  Vec3 rgb;
  f32 a;
};

// Bilinear tap, returns linear rgb + coverage alpha.
Texel SampleTex(const DecodedTex& t, f32 u, f32 v) {
  auto wrap = [](f32 x) { return x - std::floor(x); };
  f32 fx = wrap(u) * t.w - 0.5f, fy = wrap(v) * t.h - 0.5f;
  i32 x0 = static_cast<i32>(std::floor(fx)), y0 = static_cast<i32>(std::floor(fy));
  f32 tx = fx - x0, ty = fy - y0;
  auto at = [&](i32 x, i32 y) {
    x = ((x % (i32)t.w) + t.w) % t.w;
    y = ((y % (i32)t.h) + t.h) % t.h;
    const f32* p = &t.rgba[(static_cast<size_t>(y) * t.w + x) * 4];
    return Texel{{p[0], p[1], p[2]}, p[3]};
  };
  Texel a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
  auto lerp = [](const Texel& p, const Texel& q, f32 s) {
    return Texel{p.rgb + (q.rgb - p.rgb) * s, p.a + (q.a - p.a) * s};
  };
  return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);
}

// --- union-find -----------------------------------------------------------

u32 Find(base::Vector<u32>& parent, u32 x) {
  while (parent[x] != x) {
    parent[x] = parent[parent[x]];
    x = parent[x];
  }
  return x;
}
void Union(base::Vector<u32>& parent, u32 a, u32 b) {
  a = Find(parent, a);
  b = Find(parent, b);
  if (a != b) parent[a] = b;
}

// A connected group of hair cards sharing vertices, with its length ("along")
// UV axis resolved. The scalp end is `a_root`, the tip end `a_tip`.
struct Comp {
  base::Vector<u32> verts;
  base::Vector<u32> root_verts;  // verts on the scalp-side root edge (seed points)
  bool along_is_u = false;
  f32 a_root = 0, a_tip = 0;
  f32 c_min = 0, c_max = 0;
  f32 hw = 0.02f;        // world proximity bandwidth for the tracing chain
  Vec3 normal{0, 0, 1};  // card plane normal (bulges the lock cross-section out)
  Vec3 across_dir{1, 0, 0};  // unit world dir of increasing across-UV
  f32 across_world = 0;  // world width of the card across its length axis
  f32 world_per_c = 0;   // metres of card width per across-UV unit
};

inline f32 AlongOf(const Comp& comp, const GVert& v) { return comp.along_is_u ? v.u : v.v; }
inline f32 AcrossOf(const Comp& comp, const GVert& v) { return comp.along_is_u ? v.v : v.u; }

// One point of a strand: a scattered-data average of the component's vertices
// weighted by along-distance, across-distance and world proximity to the
// previous point, so the chain follows one card strip even through welded
// meshes (parallel strips at the same UV are separated by the world term).
Vec3 ChainStep(const Comp& comp, const base::Vector<GVert>& verts, f32 target_a, f32 c,
               const Vec3& prev, f32 ha, f32 hc) {
  Vec3 acc{0, 0, 0};
  f32 wsum = 0;
  f32 inv_ha = 1.0f / (ha * ha);
  f32 inv_hc = 1.0f / (hc * hc);
  f32 inv_hw = 1.0f / (comp.hw * comp.hw);
  for (u32 vi : comp.verts) {
    const GVert& gv = verts[vi];
    f32 da = AlongOf(comp, gv) - target_a;
    f32 dc = AcrossOf(comp, gv) - c;
    Vec3 dw = gv.p - prev;
    f32 e = da * da * inv_ha + dc * dc * inv_hc + Dot(dw, dw) * inv_hw;
    f32 w = std::exp(-e);
    acc = acc + gv.p * w;
    wsum += w;
  }
  return wsum > 1e-12f ? acc * (1.0f / wsum) : prev;
}

// Root-edge position at across-coordinate c: a Gaussian interp over the (sparse)
// root-band verts. Cards are wide sheets with only a handful of root verts, so we
// interpolate the scalp edge continuously instead of snapping to those few verts.
Vec3 RootAt(const Comp& comp, const base::Vector<GVert>& verts, f32 c) {
  f32 n = static_cast<f32>(comp.root_verts.size());
  f32 h = std::max((comp.c_max - comp.c_min) / std::max(1.0f, n - 1.0f), 1e-4f) * 1.2f;
  f32 inv = 1.0f / (h * h);
  Vec3 acc{0, 0, 0};
  f32 wsum = 0;
  for (u32 vi : comp.root_verts) {
    f32 dc = AcrossOf(comp, verts[vi]) - c;
    f32 w = std::exp(-dc * dc * inv);
    acc = acc + verts[vi].p * w;
    wsum += w;
  }
  return wsum > 1e-12f ? acc * (1.0f / wsum) : verts[comp.root_verts[0]].p;
}

}  // namespace

bool BuildHairGroom(const asset::Mesh& mesh, const GroomParams& params, GroomData* out) {
  if (mesh.lods.empty()) return false;
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.vertices.empty() || lod.indices.size() < 3) return false;

  base::Vector<GVert> verts;
  verts.resize(lod.vertices.size());
  for (size_t i = 0; i < lod.vertices.size(); ++i) {
    verts[i].p = ToEngine(lod.vertices[i].position);
    verts[i].u = lod.vertices[i].uv[0];
    verts[i].v = lod.vertices[i].uv[1];
  }
  base::Vector<Tri> tris;
  tris.reserve(lod.indices.size() / 3);
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    Tri t{lod.indices[i], lod.indices[i + 1], lod.indices[i + 2]};
    if (t.a == t.b || t.b == t.c || t.a == t.c) continue;
    tris.push_back(t);
  }
  if (tris.empty()) return false;

  // Connected components over shared vertices = individual cards / clumps.
  base::Vector<u32> parent;
  parent.resize(verts.size());
  for (u32 i = 0; i < verts.size(); ++i) parent[i] = i;
  for (const Tri& t : tris) {
    Union(parent, t.a, t.b);
    Union(parent, t.a, t.c);
  }
  base::UnorderedMap<u32, u32> root_to_comp;
  base::Vector<Comp> comps;
  for (const Tri& t : tris) {
    u32 root = Find(parent, t.a);
    if (!root_to_comp.find(root)) {
      root_to_comp.emplace(root, static_cast<u32>(comps.size()));
      comps.push_back(Comp{});
    }
  }
  // Collect each component's unique vertices.
  base::Vector<u32> vert_comp;
  vert_comp.resize(verts.size());
  for (u32 i = 0; i < verts.size(); ++i) vert_comp[i] = *root_to_comp.find(Find(parent, i));
  for (u32 i = 0; i < verts.size(); ++i) comps[vert_comp[i]].verts.push_back(i);

  // Groom centre: orients each card normal outward so the lock bulge pushes away
  // from the head, never into it.
  Vec3 mesh_center{0, 0, 0};
  for (const GVert& gv : verts) mesh_center = mesh_center + gv.p;
  mesh_center = mesh_center * (1.0f / verts.size());

  // Per component: length axis, scalp/tip ends, across range, world bandwidth.
  for (u32 comp_index = 0; comp_index < comps.size(); ++comp_index) {
    Comp& comp = comps[comp_index];
    if (comp.verts.empty()) continue;
    // Least-squares plane fit p ~= p0 + a*u + b*v: robust world span per UV axis,
    // no signed-gradient cancellation on curved cards (which mis-picked the axis
    // and traced the short way -> "wood shavings"). Solve the centred 2x2 system.
    f32 ubar = 0, vbar = 0;
    Vec3 pbar{0, 0, 0};
    for (u32 vi : comp.verts) {
      ubar += verts[vi].u; vbar += verts[vi].v; pbar = pbar + verts[vi].p;
    }
    f32 inv_n = 1.0f / comp.verts.size();
    ubar *= inv_n; vbar *= inv_n; pbar = pbar * inv_n;
    f32 suu = 0, svv = 0, suv = 0;
    Vec3 sup{0, 0, 0}, svp{0, 0, 0};
    Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
    for (u32 vi : comp.verts) {
      f32 du = verts[vi].u - ubar, dv = verts[vi].v - vbar;
      Vec3 dp = verts[vi].p - pbar;
      suu += du * du; svv += dv * dv; suv += du * dv;
      sup = sup + dp * du; svp = svp + dp * dv;
      bmin = {std::min(bmin.x, verts[vi].p.x), std::min(bmin.y, verts[vi].p.y),
              std::min(bmin.z, verts[vi].p.z)};
      bmax = {std::max(bmax.x, verts[vi].p.x), std::max(bmax.y, verts[vi].p.y),
              std::max(bmax.z, verts[vi].p.z)};
    }
    f32 det = suu * svv - suv * suv;
    Vec3 dpu, dpv;  // world metres per unit u / v
    if (std::fabs(det) > 1e-12f) {
      dpu = (sup * svv - svp * suv) * (1.0f / det);
      dpv = (svp * suu - sup * suv) * (1.0f / det);
    } else {
      dpu = sup; dpv = svp;
    }
    f32 amin = 1e30f, amax = -1e30f, cmin = 1e30f, cmax = -1e30f;
    // Compare world span (metres/uv * uv-extent) on each axis to pick the length.
    f32 umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    for (u32 vi : comp.verts) {
      umin = std::min(umin, verts[vi].u); umax = std::max(umax, verts[vi].u);
      vmin = std::min(vmin, verts[vi].v); vmax = std::max(vmax, verts[vi].v);
    }
    f32 span_u = Length(dpu) * (umax - umin);
    f32 span_v = Length(dpv) * (vmax - vmin);
    comp.along_is_u = span_u > span_v;
    for (u32 vi : comp.verts) {
      f32 a = AlongOf(comp, verts[vi]);
      f32 c = AcrossOf(comp, verts[vi]);
      amin = std::min(amin, a); amax = std::max(amax, a);
      cmin = std::min(cmin, c); cmax = std::max(cmax, c);
    }
    comp.c_min = cmin;
    comp.c_max = cmax;
    // World width of the card across its length axis: bandwidth that lets the
    // trace follow one strip's curvature without jumping to a parallel strip.
    Vec3 across_grad = comp.along_is_u ? dpv : dpu;
    comp.world_per_c = Length(across_grad);
    comp.across_dir = comp.world_per_c > 1e-9f ? across_grad * (1.0f / comp.world_per_c)
                                               : Vec3{1, 0, 0};
    comp.across_world = comp.world_per_c * (cmax - cmin);
    comp.hw = std::clamp(0.5f * comp.across_world, 0.006f, 0.05f);
    Vec3 nrm = Cross(dpu, dpv);
    f32 nl = Length(nrm);
    comp.normal = nl > 1e-9f ? nrm * (1.0f / nl) : Vec3{0, 0, 1};
    if (Dot(comp.normal, pbar - mesh_center) < 0) comp.normal = comp.normal * -1.0f;
    // Root = the along-end that sits higher up (the scalp): compare mean Y of
    // verts near each end.
    f32 y_lo = 0, y_hi = 0;
    u32 n_lo = 0, n_hi = 0;
    f32 band = 0.2f * (amax - amin) + 1e-6f;
    for (u32 vi : comp.verts) {
      f32 a = AlongOf(comp, verts[vi]);
      if (a < amin + band) { y_lo += verts[vi].p.y; ++n_lo; }
      if (a > amax - band) { y_hi += verts[vi].p.y; ++n_hi; }
    }
    y_lo = n_lo ? y_lo / n_lo : 0;
    y_hi = n_hi ? y_hi / n_hi : 0;
    if (y_lo >= y_hi) { comp.a_root = amin; comp.a_tip = amax; }
    else { comp.a_root = amax; comp.a_tip = amin; }
    // Root-edge verts: the scalp-side band. Strands are seeded only here so they
    // start at the scalp and trace the full card length, never mid-card.
    f32 root_band = 0.14f * std::fabs(comp.a_tip - comp.a_root) + 1e-6f;
    for (u32 vi : comp.verts) {
      if (std::fabs(AlongOf(comp, verts[vi]) - comp.a_root) <= root_band) {
        comp.root_verts.push_back(vi);
      }
    }
  }

  // Seed pool over CARDS weighted by width: strands spread continuously across
  // each card's root edge (cards are wide sheets with only a few root verts, so
  // seeding at those verts clustered strands into discrete slabs -> facets).
  base::Vector<f32> card_cum;
  base::Vector<u32> card_idx;
  f32 card_total = 0;
  for (u32 ci = 0; ci < comps.size(); ++ci) {
    const Comp& comp = comps[ci];
    if (comp.a_root == comp.a_tip || comp.root_verts.empty() || comp.across_world <= 0) continue;
    card_total += comp.across_world;
    card_cum.push_back(card_total);
    card_idx.push_back(ci);
  }
  if (card_idx.empty()) return false;

  DecodedTex diffuse;
  bool have_tex = params.diffuse && DecodeDiffuse(*params.diffuse, 256, &diffuse);
  const f32 kAlphaMin = 0.33f;  // reject roots over transparent (non-hair) texels

  u32 rng = params.seed * 2654435761u + 1u;
  auto randf = [&]() {
    rng = rng * 1664525u + 1013904223u;
    return static_cast<f32>(rng >> 8) / 16777216.0f;
  };

  const u32 P = kGroomPointsPerStrand;
  base::Vector<f32>& pts = out->points;
  base::Vector<f32>& roots = out->roots;
  base::Vector<f32>& cols = out->colors;
  pts.resize(static_cast<size_t>(params.guide_count) * P * 3);
  roots.resize(static_cast<size_t>(params.guide_count) * 3);
  cols.resize(static_cast<size_t>(params.guide_count) * 3);

  Vec3 root_sum{0, 0, 0};
  f64 length_sum = 0;
  u32 emitted = 0;
  u32 attempts = 0;
  const u32 max_attempts = params.guide_count * 8;
  while (emitted < params.guide_count && attempts < max_attempts) {
    ++attempts;
    f32 rc = randf() * card_total;
    u32 lo = 0, hi = static_cast<u32>(card_idx.size()) - 1;
    while (lo < hi) {
      u32 mid = (lo + hi) / 2;
      if (card_cum[mid] < rc) lo = mid + 1; else hi = mid;
    }
    const Comp& comp = comps[card_idx[lo]];
    f32 c_seed = comp.c_min + randf() * (comp.c_max - comp.c_min);
    Vec3 root_pos = RootAt(comp, verts, c_seed);

    // Alpha-weighted colour from taps a bit down the strand; reject the seed if
    // the card is transparent there (background beige texels never colour hair).
    static const f32 taps[] = {0.08f, 0.20f, 0.34f, 0.50f};
    Vec3 col_acc{0, 0, 0};
    f32 a_acc = 0;
    for (f32 tf : taps) {
      f32 a = comp.a_root + (comp.a_tip - comp.a_root) * tf;
      f32 uu = comp.along_is_u ? a : c_seed;
      f32 vv = comp.along_is_u ? c_seed : a;
      Texel s = SampleTex(diffuse, uu, vv);
      col_acc = col_acc + s.rgb * s.a;
      a_acc += s.a;
    }
    if (have_tex && a_acc < kAlphaMin * 4.0f) continue;
    Vec3 color = (have_tex && a_acc > 1e-3f) ? col_acc * (1.0f / a_acc) : Vec3{1, 1, 1};

    f32 ha = 0.8f * std::fabs(comp.a_tip - comp.a_root) / (P - 1) + 1e-4f;
    f32 hc = 0.06f * std::fabs(comp.c_max - comp.c_min) + 1e-4f;
    // Per-strand flyaway: a fixed random bend that grows toward the tip, applied
    // to the stored point only (not the chain anchor) so neighbouring strands from
    // the same card fan apart into individual locks instead of a flat card-slab.
    f32 fang = randf() * 6.2831853f;
    f32 fh = randf();
    Vec3 fdir{std::cos(fang), -0.15f - 0.3f * fh, std::sin(fang)};  // outward + slight droop
    // A few strands only (fh near 1) get a real flyaway; most stay tidy so the
    // silhouette reads as groomed hair, not frizz. Curl already separates strands.
    f32 famp = std::clamp(comp.hw * 0.3f, 0.002f, 0.008f) * (fh > 0.7f ? fh : 0.15f * fh);
    // Lock cross-section: quantise across into ~10 mm locks and bulge each out
    // along the card normal (round tube, not flat sheet).
    const f32 kLockWorld = 0.010f;
    f32 lock_c = comp.world_per_c > 1e-6f ? kLockWorld / comp.world_per_c : (comp.c_max - comp.c_min);
    lock_c = std::min(std::max(lock_c, 1e-6f), comp.c_max - comp.c_min + 1e-6f);
    f32 lock_center = comp.c_min + (std::floor((c_seed - comp.c_min) / lock_c) + 0.5f) * lock_c;
    f32 an = std::clamp((c_seed - lock_center) / (0.5f * lock_c), -1.0f, 1.0f);
    f32 profile = std::sqrt(std::max(0.0f, 1.0f - an * an));
    f32 bulge = 0.5f * lock_c * comp.world_per_c * (profile * 0.9f + (randf() - 0.4f) * 0.4f);
    // Per-strand curl: a two-frequency wave (in-plane + out-of-plane) unique to
    // each strand, growing toward the tip, so coplanar strands of one flat card
    // wave apart into distinct flowing strands instead of one lit facet.
    f32 cph1 = randf() * 6.2831853f, cph2 = randf() * 6.2831853f;
    f32 cph3 = randf() * 6.2831853f, cph4 = randf() * 6.2831853f;
    f32 cfreq = 4.0f + randf() * 6.0f;
    // Curl scaled to lock width with a floor, so the smaller-card style (elf
    // blonde) breaks up as much as the wide-card ones. Two octaves: a broad wave
    // plus fine detail that breaks flat card facets.
    f32 camp = std::clamp(comp.hw * 0.85f, 0.009f, 0.024f) * (0.6f + 0.8f * randf());
    // Trace root->tip, anchored on the scalp vertex and holding across = c_seed so
    // it follows one card strip (narrow world bandwidth prevents slab merging).
    Vec3 chain_prev = root_pos;
    Vec3 pt_prev = root_pos;
    f32 len = 0;
    for (u32 k = 0; k < P; ++k) {
      Vec3 pos;
      if (k == 0) {
        pos = root_pos;
      } else {
        f32 tt = static_cast<f32>(k) / (P - 1);
        f32 a = comp.a_root + (comp.a_tip - comp.a_root) * tt;
        chain_prev = ChainStep(comp, verts, a, c_seed, chain_prev, ha, hc);
        f32 ramp = std::min(1.0f, tt / 0.18f);  // keep roots on the scalp
        f32 wv = camp * tt;
        Vec3 curl =
            comp.normal * ((std::sin(cfreq * tt + cph1) + 0.4f * std::sin(cfreq * 2.7f * tt + cph3)) * wv) +
            comp.across_dir *
                ((std::sin(cfreq * 0.7f * tt + cph2) + 0.4f * std::sin(cfreq * 2.3f * tt + cph4)) * wv * 0.6f);
        pos = chain_prev + comp.normal * (bulge * ramp) + curl + fdir * (famp * std::pow(tt, 1.4f));
      }
      pts[(static_cast<size_t>(emitted) * P + k) * 3 + 0] = pos.x;
      pts[(static_cast<size_t>(emitted) * P + k) * 3 + 1] = pos.y;
      pts[(static_cast<size_t>(emitted) * P + k) * 3 + 2] = pos.z;
      if (k == 0) {
        roots[emitted * 3 + 0] = pos.x;
        roots[emitted * 3 + 1] = pos.y;
        roots[emitted * 3 + 2] = pos.z;
      } else {
        len += Length(pos - pt_prev);
      }
      pt_prev = pos;
    }
    root_sum = root_sum + root_pos;
    cols[emitted * 3 + 0] = color.x;
    cols[emitted * 3 + 1] = color.y;
    cols[emitted * 3 + 2] = color.z;
    length_sum += len;
    ++emitted;
  }
  if (emitted == 0) return false;
  pts.resize(static_cast<size_t>(emitted) * P * 3);
  roots.resize(static_cast<size_t>(emitted) * 3);
  cols.resize(static_cast<size_t>(emitted) * 3);

  // Recentre on the scalp (mean root) so a rigid transform drops the groom onto
  // a head bone, and fit a head collision sphere just inside the scalp cap.
  Vec3 scalp = root_sum * (1.0f / emitted);
  f32 head_r = 0;
  for (u32 i = 0; i < emitted; ++i) {
    Vec3 rp{roots[i * 3], roots[i * 3 + 1], roots[i * 3 + 2]};
    f32 horiz = std::sqrt((rp.x - scalp.x) * (rp.x - scalp.x) + (rp.z - scalp.z) * (rp.z - scalp.z));
    head_r = std::max(head_r, horiz);
  }
  for (size_t i = 0; i < pts.size(); i += 3) {
    pts[i] -= scalp.x; pts[i + 1] -= scalp.y; pts[i + 2] -= scalp.z;
  }
  for (size_t i = 0; i < roots.size(); i += 3) {
    roots[i] -= scalp.x; roots[i + 1] -= scalp.y; roots[i + 2] -= scalp.z;
  }
  out->guide_count = emitted;
  out->collision_center = {0.0f, -0.35f * head_r, 0.0f};
  out->collision_radius = 0.78f * head_r;
  out->mean_length = static_cast<f32>(length_sum / emitted);
  REC_INFO("hair groom: {} cards, {} guides, mean len {:.3f}, head r {:.3f}", comps.size(),
           emitted, out->mean_length, head_r);
  return true;
}

}  // namespace rec::render
