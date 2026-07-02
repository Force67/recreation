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

- [x] **VRS.** (landed) Content-adaptive rate image on the scene pass:
      luminance-detail scored per 16px block, motion-gated (static camera
      stays near-full-rate - coarse fragments stripe glossy surfaces under
      the temporal upscaler), 2x2 ceiling. REC_VRS / REC_VRS_THRESHOLD.
- [x] **Async compute, dedicated family.** (landed) Async queue now picks a
      compute-only family (GB10: family 2; REC_ASYNC_DEDICATED=0 falls back);
      CONCURRENT sharing on buffers + non-attachment images (render targets
      stay EXCLUSIVE for compression), compute-family command pool, and
      compute-legal stage/access mask filtering on the async list's barriers.
- [x] **PSO hitch elimination.** (landed) The persistent pipeline cache fixes
      run 2+; startup pipeline creation now batches onto a worker pool
      (BeginPipelineBatch/EndPipelineBatch, binds wait on in-flight compiles)
      so a cold cache costs ~90 ms over warm instead of ~2.6 s. REC_PSO_BATCH=0
      forces the serial path.
- [x] **Golden-image regression CI.** (landed) tests/golden/golden.py:
      REC_FIXED_DT lockstep captures of 4 demo scenes vs checked-in refs
      (NVIDIA baseline, half-res), diff heatmaps; CI golden-smoke job on
      lavapipe (smoke-level until a runner ref set is promoted).
- [x] **Histogram auto-exposure.** (landed) Average-based metering is why demos pin
      exposure manually; histogram + center weighting + adaptation rates.

## Tier 3 — moonshots (multi-session each)

- [x] **Virtual geometry** (landed - core) — cluster-DAG LOD: QEM
      simplifier with locked group borders (engine/asset/simplify.cc), DAG
      build via morton-grouped meshlets re-clustered per group so a cluster
      and its replacement share the exact (sphere, error) pair (gap-free cut
      by construction), mesh-shader runtime cut
      project(self) <= tau < project(parent) + frustum/backface cull.
      --demo vgeo: 819k-tri terrain -> 29.8k clusters / 5 levels, cluster
      count scales with REC_VGEO_ERROR. Next: scene-path integration,
      streaming, software raster, virtual shadow maps.
- [x] **Virtual texturing** (landed - core) — feedback-driven sparse VT:
      256x256-page virtual space (9 mips) behind a mip-mapped indirection
      texture + 4096^2 page atlas with 4px filter gutters, fragment-shader
      request feedback (rotating 1/64 pixel subset), worker-thread page
      generation, LRU eviction (engine/render/texturing/). Materials opt in
      via virtual_albedo; --demo vt streams a procedural survey megatexture.
      Next: real content backing (disk pages), non-albedo channels.
- [x] **Hybrid-path ReSTIR DI** (landed; GI still recon-only) — per-pixel
      reservoirs over the clustered point/spot lights on the prepass G-buffer
      (temporal + spatial reuse, one ray-query shadow ray for the winner),
      demodulated diffuse/spec textures folded into the forward pass (env
      slots 23/24, kFrameFlagRestirDi). Replaces the analytic cluster loop +
      local shadow atlas for those lights on opaque surfaces. Experimental,
      REC_RESTIR_DI=1 (~0.3 ms, lights demo).
- [x] **Strand-based hair** (landed - core) — 4096 verlet guide strands
      (one compute thread per strand: gravity/wind, inextensibility
      iterations, head-sphere collision), rendered as camera-facing ribbons
      expanded in the VS from the sim buffer, dual-lobe Kajiya-Kay shading.
      --demo strands. Real characters plug in by seeding roots from scalp
      geometry; next: interpolated child strands, strand self-shadowing.

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
