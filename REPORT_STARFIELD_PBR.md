# Starfield PBR metallic/roughness/AO shading

Goal: make reflective/glossy Starfield surfaces (New Atlantis architecture, glass
facades, metal) read reflective instead of flat matte grey, by wiring Starfield's
material values into a proper metallic/roughness/AO raster path in the rx engine.

Two repos:
- rx engine (core feature): `/home/vince/Documents/Projects/rx`, branch `feat/pbr-metallic-roughness`, commit `e42e650`.
- recreation (Starfield wiring): `/home/vince/Documents/Projects/recreation`, branch `feature/starfield-pbr`, commit `082818d`.

Neither `main` was touched; nothing pushed; no PRs.

## 1. Audit (what already existed vs what was missing)

The rx **raster mesh shader is already a full metallic/roughness PBR pipeline** and
was doing far more than "flat matte grey" would suggest:

- `engine/render/shaders/pipeline/mesh.ps.hlsl` + `mesh_rt.ps.hlsl` already read
  metallic + roughness (glTF ORM `.gb` packing, slot 3), run Cook-Torrance GGX
  specular from the sun, tint specular by base color for metals, kill diffuse for
  metals (`f0 = lerp(dielectric, albedo, metallic)`, `diffuse = albedo*(1-metallic)`),
  and add a **roughness-aware split-sum IBL ambient specular** (prefiltered sky
  cube + BRDF LUT + Fdez-Aguera multi-scatter). IBL is on by default for exteriors
  (`settings.ibl = true`, `engine/render/core/settings.h`). The RT variant also has
  NRD-denoised stochastic reflections.
- `asset::Material` already had `metallic_factor` (default 0), `roughness_factor`
  (default 1), and a combined `metallic_roughness` texture slot.
- The GPU `MaterialSystem::Params` + bindless `MaterialRecord` already carried
  metallic/roughness scalars and the combined mr texture.

So the shading math was there. Two real gaps:

1. **rx**: the raster path read metallic AND roughness from a *single combined glTF
   ORM texture* (`metallic_roughness_map.gb`). There was no separate metallic
   texture slot and no per-material ambient-occlusion texture slot (only a
   screen-space AO buffer). Starfield ships **separate** slot-3 roughness, slot-4
   metallic, slot-5 AO textures.
2. **recreation**: `StarfieldMaterialDb` (`engine/bethesda/material_db.cc`) only
   resolved TextureSet slots 0/1/7 (color/normal/emissive) - roughness/metal/AO
   were read into a scratch array during the graph walk then dropped.
   `ConvertStarfieldNif` (`engine/bethesda/converters.cc`) hardcoded
   `roughness_factor = 0.8, metallic_factor = 0` and bound only color/normal/
   emissive. So every Starfield surface was a fixed rough dielectric - matte grey.

## 2. rx engine changes (`feat/pbr-metallic-roughness`, general, any game)

Added dedicated **metallic** and **ambient-occlusion** texture slots for engines
that ship separate maps instead of packed ORM. All defaults neutral so combined-ORM
and untextured content shade EXACTLY as before.

- `engine/asset/material.h`: new `metallic_map`, `occlusion_map` AssetId slots, a
  `bool separate_metallic` (default false), and `f32 ao_strength` (default 1.0).
- `engine/render/pipeline/material_system.{h,cc}`: material set 1 extended from 6 to
  8 bindings (slot 6 metallic, slot 7 occlusion); `Params.ao_strength`;
  `map_keys[5] -> map_keys[7]`; two new flags `kFlagSeparateMetallic` (1<<16) and
  `kFlagHasOcclusion` (1<<17). Missing metallic/occlusion maps fall back to a 1x1
  white default (white metallic * factor 0 = dielectric; white occlusion = no AO).
- `engine/render/shaders/pipeline/mesh.ps.hlsl` + `mesh_rt.ps.hlsl`: sample the two
  new maps. Under `kFlagSeparateMetallic` the mr slot is treated as roughness-only
  (`.g`) and metallic comes from `metallic_map.r`; under `kFlagHasOcclusion` the
  occlusion map multiplies the indirect/ambient term (`lerp(1, mao, ao_strength)`).
  The SSS pixel-shader variants `#include` these two shaders, so they inherit it.

Metals therefore reflect the prefiltered physical sky (already baked every frame),
tint that reflection by base color, and drop diffuse - the reflective look - with no
new IBL/probe system required.

## 3. recreation Starfield wiring (`feature/starfield-pbr`)

- `engine/bethesda/material_db.{h,cc}`: resolve TextureSet slots 3/4/5 (roughness/
  metallic/AO) from the CDB object graph (`tex_of` already returns any slot) and add
  `_rough`/`_metal`/`_ao` suffix rules to the stem-index fallback. New
  `Resolved` struct + `Lookup(path, Resolved*)` overload; the CDB exposes no scalar
  metalness through the TextureFile leaves this reader follows, so a bound metallic
  map is the metal signal (`metallic_hint`). The legacy 3-arg `Lookup` is kept
  (material_dbtest still uses it and passes).
- `engine/bethesda/converters.cc`: `BindStarfieldMaterial` / `BindConventionTextures`
  now bind roughness -> the `metallic_roughness` slot, metallic -> `metallic_map`
  (with `separate_metallic = true`), AO -> `occlusion_map`. A bound metallic map or
  the CDB metal hint promotes the surface to a metal (`metallic_factor = 1`); a real
  roughness map lets `roughness_factor` pass through at 1.0 instead of the gray-
  default 0.8. `PathIsLinearData` extended so `_rough`/`_metal`/`_ao` UNORM DDS are
  sampled linear, not sRGB. `RX_STARFIELD_PBR=0` pins the old gray-matte look for A/B.
- `engine/world/cell_streaming.cc`: upload the metallic + occlusion textures so they
  are resident before the material-record flags (gated on texture presence) resolve.

## 4. Screenshots (before/after)

All captured at 1920x1080 via `RX_SCREENSHOT`, fixed `RX_CAM`, `RX_DISTANT_LOD=1`,
`RX_WEATHER=pleasant`, `RX_HIDE_DEBUG_UI=1`, t=26s. Stored in `./.pbr_shots/`.
Before = same binary with `RX_STARFIELD_PBR=0`.

### Starfield reflectivity (the feature)
- Aerial New Atlantis (`RX_CAM="150,180,480,5.8,-0.35"`):
  - `.pbr_shots/starfield_aerial_before.png`
  - `.pbr_shots/starfield_aerial_after.png`
- Ground New Atlantis (`RX_CAM="120,90,300,5.6,-0.08"`):
  - `.pbr_shots/starfield_ground_before.png`
  - `.pbr_shots/starfield_ground_after.png`
- Amplified difference (contrast x4): `.pbr_shots/starfield_ground_diff.png`,
  `.pbr_shots/starfield_aerial_diff.png`. The diff is confined to the *buildings*
  (MAST tower, structures, bridge pylon, cistern tanks) - terrain, water and sky are
  untouched, confirming the change is correctly scoped to architecture materials.

Result: the MAST tower and facades go from uniform dark matte grey to brighter,
panel-varied surfaces with a sky-tinted specular sheen; metal pieces (tanks, bridge
pylon) pick up a metallic reflection. The effect is physically tasteful (most
architecture roughness maps are semi-glossy), not blown out.

### Non-regression (defaults must preserve the look)
- Skyrim Whiterun (`RX_CAM="321.7,60,400,0,-0.38"`): `.pbr_shots/skyrim_after.png` -
  unchanged. Skyrim NIF materials never set `separate_metallic` and bind no
  metallic/AO maps, so they take the identical shading path as before.
- Fallout 4 Boston (`RX_CAM="321.7,60,400,0,-0.38"`): `.pbr_shots/fo4_after.png` -
  unchanged. FO4 BGSM sets only `roughness_factor` (as before) and never sets the new
  flags/slots.

## 5. Build / test status

- Build: recreation compiled against the sibling rx via the exact flake command
  (three `--override-input` path inputs incl. `rx-src`), `RelWithDebInfo`, exit 0.
- Tests: `ctest` 75/75 pass (incl. `material_dbtest`, `dds_srgb_fo4test`,
  `bgsm_fo4test`).

## 6. What remains / honest notes

- Reflections are IBL-based (prefiltered physical sky cube), not screen-space or RT,
  on the raster path. That is the engine's existing ambient-specular mechanism and is
  correct for a distant-sky reflection; it does not reflect nearby geometry. The RT
  variant (`mesh_rt.ps`) already routes glossy surfaces through NRD-denoised
  stochastic reflections, so with ray tracing on, metals also pick up local geometry
  - no extra work was needed there and both shaders got the same metallic/AO change.
- The CDB reader infers "metal" from a bound slot-4 metallic map (no scalar metalness
  is exposed through the TextureFile leaves it follows). Materials that are metal but
  ship no metallic map stay dielectric. Extracting the scalar metalness/roughness
  BSMaterial components would tighten this but needs more CDB graph parsing.
- Stem-fallback suffixes (`_rough`/`_metal`/`_ao`) cover the common convention;
  non-standard suffixes on graph-missed materials would be dropped.
