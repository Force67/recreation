# Renderer roadmap — path to AAA

Gap analysis as of 2026-07-02 (branch `feature/rhi-backend-agnostic`). The
foundation already in place: RHI over Vulkan 1.3 + D3D12, bindless, frame
graph, async-compute fork/join, DDGI + RTAO + denoised RT reflections + NRD,
path tracing with ReSTIR GI/DI, clustered lights (spot/sphere/LTC rect) and
decals, CSM + RT sun shadows + contact + cloud shadows, extended PBR
(clearcoat/anisotropy/sheen/iridescence/transmission/POM), screen-space SSS,
Kajiya-Kay hair, Hillaire sky + volumetric clouds + aerial perspective,
Gerstner water + caustics, TAA/FSR3/DLSS + FSR3 frame generation, AgX + lens
package + HDR10/scRGB output, GPU culling + meshlets + auto LOD + distant LOD
streaming.

## Tier 1 — correctness gaps that read as "not AAA" immediately

- [x] **Shadowed local lights.** (landed) Clustered spot/point lights cast no shadows
      in the raster path (light leaks through walls — the biggest tell).
      Shadow atlas with per-face slots (spot = 1 face, point = 6 cube faces),
      nearest-K selection, PCF sampling in the cluster loop. Reuse the CSM
      depth pipeline for face rendering; `PointLight.params.w` carries the
      atlas slot (free across all light types).
- [x] **Unified froxel volumetric lighting.** (landed) A 3D scattering volume fed by
      the sun (shadowed) + all clustered lights (with their new shadows),
      temporally jittered, integrated front-to-back; fog composite samples it,
      and particles/translucents sample the same volume so a torch lights the
      smoke above it. The current fog pass only marches the sun.
- [x] **Decal channels.** (landed) Clustered decals blend albedo only; add
      normal/roughness/emissive perturbation (wet stains with glossy
      interiors, glowing runes, impact craters).
- [x] **Lit translucency.** (landed) Particles and WBOIT get sun+ambient only; loop
      the light clusters and sample the froxel volume (transmittance +
      inscatter) at their depth.

## Tier 2 — performance & production maturity

- [ ] **VRS.** `caps().fragment_shading_rate` is detected and unused.
      Content-adaptive shading-rate image (luminance + motion driven) is
      typically a free 10-20% on the scene pass.
- [ ] **Async compute, dedicated family.** The fork/join infra is in; on
      NVIDIA the same-family second queue does not overlap. Move to the
      compute-only family with queue-family ownership transfers (or
      CONCURRENT sharing on crossed resources incl. bindless materials).
- [ ] **PSO hitch elimination.** The persistent pipeline cache fixes run 2;
      async pipeline compilation + ubershader fallback fixes run 1.
- [ ] **Golden-image regression CI.** lavapipe + REC_UI_SHOT + per-demo
      reference images with tolerances; every ingredient exists.
- [x] **Histogram auto-exposure.** (landed) Average-based metering is why demos pin
      exposure manually; histogram + center weighting + adaptation rates.

## Tier 3 — moonshots (multi-session each)

- [ ] **Virtual geometry** (Nanite-class): cluster-DAG simplification,
      streaming, software-raster micro-poly path. Meshlets + task culling +
      occlusion already exist as the foundation. Pairs with **virtual shadow
      maps** replacing the CSM.
- [ ] **Virtual texturing** — scalability for 4K-modded content.
- [ ] **Hybrid-path ReSTIR GI/DI** — bring reservoir many-light sampling from
      the path tracer into the hybrid path as the clustered-light shading
      input (the Lumen-competitive direction).
- [ ] **Strand-based hair** (compute-simulated) once real character assets
      exist; Kajiya-Kay cards are in place.

## Loose ends

- [ ] LTC for sphere lights (rect done; sphere still representative-point).
- [ ] FFT ocean (Tessendorf) to replace Gerstner; shoreline flow maps.
- [ ] Foliage imposters at distance; hierarchical pivot wind.
- [ ] XeSS (enum exists, unimplemented).
- [ ] D3D12 Windows runtime validation (vkd3d parity holds on Linux).
- [ ] DLSS-RR pending NVIDIA aarch64 snippets.
- [ ] Skinned decals (wounds) once characters land.
- [ ] Shutdown leak: 3x VUID-vkDestroyDevice-05137 child objects (pre-dates
      frame generation; visible in --demo cube validation runs).
