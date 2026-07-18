# Proposal: Adopting the AC Shadows RTGI techniques in rx + recreation

Source: "Ray tracing the world of Assassin's Creed Shadows" (Ubisoft Montréal, SIGGRAPH 2025
Advances in Real-Time Rendering). Reviewed against rx `main` @ 63d5a02 and recreation `main`
@ 26ee8db (worktrees `rx-acshadows-review` / `recreation-acshadows-review`, branch
`review/ac-shadows-stack`).

---

## 1. Where we already are

AC Shadows' architecture is: per-pixel diffuse + specular rays (screen-space march first,
coarse BVH second), a cascaded probe volume as the secondary-bounce radiance/irradiance
cache, an NRD-style denoiser, and a heavily simplified RT scene description. Our direct
analog is **RCGI** (`rx: engine/render/gi/rcgi.{h,cc}`, `RCGI.md`), which is in some ways a
generation ahead of their probe volume:

| AC Shadows | rx today | Verdict |
|---|---|---|
| 5-cascade 16×16×8 probe volume, toroidal, octahedral atlases | RCGI irradiance cascades: 4 × 16³ octahedral probes, camera-snapped, round-robin 1 cascade/frame | **Have** |
| Radiance cache probes as ray-miss fallback | Spatial-hash **world radiance cache at hit points** (1M cells, distance-LOD cell size) — finer-grained than probe radiance | **Have (better)** |
| Multi-bounce via previous-frame irradiance | Same (cache shade reads previous-frame cascades) | **Have** |
| VSM (Chebyshev) probe visibility weights | Same (mean/mean² moments atlas, Chebyshev) | **Have** |
| Local lights at secondary hits | Cascaded world-space **light grid** (16³ × 4, ≤256 lights) — same purpose | **Have** |
| Per-pixel SH encoding, evaluate with full-res normal at upscale | 2-band SH gather, bilateral upscale evaluating SH with full-res normal | **Have** |
| NRD-like modular denoiser | Real NRD (REBLUR diffuse/spec/AO, SIGMA shadow) + custom RCGI bilateral/temporal chain | **Have** |
| Cloud shadows in probe lighting | Cloud shadow map exists (`atmosphere/cloud_shadow.cs.hlsl`) | **Have** |
| BLAS memory discipline | BLAS compaction (~50% savings), per-instance ray masks | **Have** |
| Console/no-RT scalability via baked GI | **SDF software-trace tier** (`RX_RCGI_SW`) — fully dynamic, no bake | **Have (better)** — they list "get rid of baked technique" as future work; we already have no bake |
| ReSTIR-style sampling (their future work: importance sampling) | ReSTIR DI landed (`gi/restir_di.*`), path-tracer ReSTIR GI/DI | **Have** |

So we are **not** implementing their GI algorithm — we already ship its successor. What we
should adopt is the *production engineering around it*: the coarse RT scene description,
vegetation/translucency handling, leak/occlusion hardening, and the perf-shaped tracing
pipeline. Those are exactly the areas where the surveys found gaps.

## 2. Gap analysis

### RT scene description (their §2 — biggest divergence)

| Their technique | Our state | Gap |
|---|---|---|
| Solid-angle + distance instance culling, time-sliced (<1 s full sweep) | TLAS takes **every** visible draw + all static instance groups, no RT-specific culling (`renderer.cc` TLAS loop ~2681) | **Open** |
| Low LODs in BVH, per-platform BVH quality | `force_lod0_for_tlas` — LOD0 everywhere in RT | **Open** |
| Per-triangle baked data (face normal + 3 UVs, one 4×32-bit fetch) | Hit shading fetches index buffer + interleaved 52-byte vertices per vertex (`rt_geometry.hlsli`) | **Open** |
| Unified simplified material, averaged PBR constants, bindless albedo/alpha only | Simplified `MaterialRecord` exists (base color, MR, emissive, bindless albedo) — already close; no averaged-constant bake | **Mostly have** |
| Opacity/alpha baking for alpha-test meshes | None; masked geometry is flagged non-opaque but all realtime rays are `FORCE_OPAQUE` | **Open** |
| Terrain patches + world-space atlas in BVH | Terrain cells are in the BVH; RT hits shade the simplified legacy splat bake (`land_baker.h`) — adequate, no world atlas | **Good enough** |
| TLAS rebuilt per frame on async queue | TLAS rebuilt per frame, ping-pong slots, but on the graphics timeline | **Partial** (async move is a perf item) |
| Season color LUTs | No season system in recreation | **Skip** (N/A) |

### Alpha-tested vegetation (their biggest content challenge)

- Their shipped solution: **opaque approximation** — scale triangles by baked average opacity,
  force opaque → 60% of reference cost, near-reference quality. Specular uses real alpha-test
  capped at 4 any-hit evaluations.
- Us: grass is `exclude_from_rt` entirely (`grass_baker.cc:273`); masked tree canopies are in
  the BVH but rays treat them as fully opaque leaves-as-solid-walls. This is our single
  largest visual gap: no vegetation in GI/reflections, or over-darkened canopies.

### Translucency (shoji doors / paper walls ≈ our interiors' thin geometry)

- Their solution: thin-translucency at secondary hits (both-face direct + split-probe indirect),
  probabilistic front/back ray selection with pdf rebalance.
- Us: blend materials are excluded from the BLAS entirely; no translucency at RT hits. Dark
  interiors behind windows/doors will show exactly the artifact their slide 91 shows.

### Light leaks & occlusion

| Their technique | Our state |
|---|---|
| Indoor/outdoor probe + hit classification via interior volumes/planes | **Missing in RCGI** — and recreation already has authoritative interior data (CELL/XCLL, `in_interior()`, interior volumes) it never forwards to the renderer |
| Probe relocation (move out of geometry, backface-count disable) | **Missing in RCGI** (DDGI-era code has none either) |
| Probe AO (`irradiance *= saturate(relHitDist*scale+bias)`) | **Missing** — we have the hitT in the gather; it's a cheap add |
| Secondary shadow map (1k², 128 m, *all* occluders, time-sliced /16 frames) for secondary-hit sun shadows | We cast real TLAS shadow rays at cache-shade points (more accurate, more expensive); no cheap fallback for the SW/SDF tier where shadow rays sphere-trace |
| GI blocker meshes for caves | **Missing** (could map to Skyrim room bounds/portals later) |
| GTAO for micro-occlusion recovery | We have SSAO + RTAO; no GTAO | 

### Per-pixel passes & perf shaping

| Their technique | Our state |
|---|---|
| Screen-space ray march **before** BVH (38% of rays resolve on screen) | RCGI gather has a screen *radiance reuse* cache but no SS march; **reflections trace the TLAS directly at full res** |
| Diffuse at quarter res | RCGI gather at half res (quarter is "a one-constant change, untuned" per `RCGI.md`) |
| Specular at half res, GGX VNDF, roughness-scaled ray length | Full res, GGX VNDF ✔, no roughness-scaled ray length, roughness cutoff 0.6 ✔ |
| Specular ray skipping: reuse diffuse SH when roughness high / direction far from mirror (20-30% saved) | **Missing** — and we have the per-pixel SH sitting right there |
| Far cubemap (128², 1 face/frame) with terrain+far geometry for miss fallback | Sky-only atmosphere cubemap; distant mountains vanish from water/mirror reflections |
| One-step fog on specular hits | **Missing** (fog pass exists, not evaluated on RT hits) |
| Radiance demodulation + luminance-compressed denoising, firefly clamp | NRD REBLUR handles demodulation/history; sky-clamp exists; luminance compression not needed unless we leave NRD |
| RTAO from the diffuse gather's hit distance | Separate RTAO pass (extra rays); could be derived free from RCGI hitT |
| Material-ID mask in spatial filter (translucency/character/vegetation) | NRD material-ID input unused; RCGI denoise has no mask |
| Vegetation disocclusion relaxation, extra Poisson pass on disoccluded pixels | NRD-internal only |

### Game side (recreation)

- **RCGI is unreferenced by recreation** — shipping default is DDGI + SSGI. The AC-Shadows-class
  system exists and is idle.
- Immutable-prop instance groups exist in rx and are being wired on the in-flight
  `feature/streamed-props` branch (commit 26c729b) — complements this plan, not part of it.
- Cell streaming, interior classification, WTHR/CLMT weather, clock-driven sun are all present
  and reusable as-is.

## 3. Proposed plan

Ordered by visual-impact-per-effort. Effort: S ≤ 1 day, M ≈ 2-4 days, L ≈ 1-2 weeks.

### Phase 1 — Vegetation in the rays (highest visual impact)

1. **Average-opacity bake for masked materials** (M, rx) — at mesh upload, rasterize each
   masked submesh's triangles into its alpha texture (or sample per-triangle UV footprint) and
   store per-submesh (or per-triangle) average opacity next to the BLAS metadata
   (`raytracing.cc` upload path).
2. **Opaque approximation BLAS variant** (M, rx) — for masked submeshes, emit a second
   geometry with triangles scaled about their centroid by average opacity, flagged opaque.
   Realtime rays keep `FORCE_OPAQUE` and get correct-on-average vegetation occlusion at their
   measured ~60% of alpha-test cost. Select via ray mask so the path tracer keeps real geometry.
3. **Include grass in the path-trace mask, evaluate for realtime** (S, recreation) — drop
   `exclude_from_rt` on grass in `grass_baker.cc` behind an env (`RX_RT_GRASS`), route it to the
   opaque-approximation path; measure before enabling by default (they *culled* small clutter —
   we should too, see Phase 2 culling).
4. **Bounded any-hit alpha-test for specular** (M, rx) — reflections switch masked geometry to
   real alpha evaluation via `RayQuery` candidate loop capped at 4 tests (their finding: the
   opaque approximation is visibly wrong in sharp reflections). Inline ray query supports this
   without SBT/anyhit shaders.

### Phase 2 — RT scene scalability (perf headroom for everything else)

5. **Solid-angle TLAS instance culling, time-sliced** (M, rx) — beyond a start distance, drop
   instances whose bounding-sphere solid angle is under a threshold; amortize the sweep over
   frames (they do the full scene in <1 s). Per-platform threshold knob.
6. **RT LOD selection** (M, rx) — replace `force_lod0_for_tlas` with "LOD0 near, LOD1+ far",
   keeping raster/RT agreement inside the screen-trace range to avoid the self-intersection
   disparity they call out in their limitations (start distance ≥ screen-trace max distance).
7. **Per-triangle baked hit data** (M, rx) — optional packed buffer per BLAS: face normal +
   3×(u16,v16) UVs, one fetch in `rt_geometry.hlsli` instead of index + 3 vertex loads. Feature
   flag; validate with the golden harness.
8. **TLAS build on the async compute queue** (S-M, rx) — we already have async infra
   (dedicated-family async landed); move the per-frame TLAS build off the graphics timeline.

### Phase 3 — Leak & occlusion hardening (interiors are recreation's bread and butter)

9. **Indoor/outdoor classification into RCGI** (L, rx + recreation) — recreation forwards
   interior volumes (interior cell bounds; later Skyrim room-bound XRGN/portal data) into a
   small plane/box list; RCGI classifies irradiance probes and cache-shade hit points, and the
   gather only blends same-class probes. This is their single most important leak fix and we
   have better source data than they did.
10. **Probe relocation + backface disable** (M, rx) — during probe trace, count backface hits;
    offset probe within its cell away from min-depth direction, disable if unrecoverable.
    Store the offset in probe metadata (they keep world-space offset per probe).
11. **Probe AO** (S, rx) — in the gather, attenuate irradiance-fallback samples by
    `saturate(hitT / cascadeSpacing * scale + bias)`. Nearly free; recovers contact occlusion
    the cascades can't represent.
12. **RTAO from gather hit distance** (S-M, rx) — feed RCGI's per-pixel hitT into the REBLUR
    occlusion denoiser instead of (or blended with) the dedicated RTAO rays; retire 2 rays/px.
13. **Secondary shadow map for the SW tier** (M, rx, optional) — 1k² top-down-ish sun map,
    128 m around camera, all occluders, time-sliced over 16 frames; used by SDF cache shade
    where TLAS shadow rays don't exist and sphere-traced shadows are soft/leaky.

### Phase 4 — Specular quality & perf

14. **Half-res reflections + bilateral upscale** (M, rx) — biggest reflection perf lever; they
    shipped half-res on PS5 Pro.
15. **Screen-space-first hybrid tracing** (L, rx) — SS ray march (linear + refine, reusing the
    SSR marcher) before the TLAS in both `reflection_trace` and the RCGI gather ray; backtrack
    to last valid SS position on failure. Their numbers: 38% resolve on screen, 13% skipped.
16. **Specular ray skipping via diffuse SH** (S, rx) — if roughness > threshold or
    `dot(rayDir, mirrorDir)` < threshold, evaluate the RCGI per-pixel SH instead of tracing.
    20-30% of specular cost for one branch.
17. **Roughness-scaled ray length** (S, rx) — `maxDist * ((1-roughness)² + 0.1)`.
18. **Far scene cubemap** (M, rx) — 128² cubemap at camera, 1 face/frame, rasterizing terrain +
    distant LOD + sky; replaces the atmosphere-only cube as the reflection/GI miss fallback so
    water reflects mountains. recreation's distant-LOD proxies (`.btr/.bto`, currently
    RT-excluded) are exactly the geometry to render into it.
19. **One-step fog on specular hits** (S, rx) — evaluate the height/froxel fog integral once
    along the reflected ray; fixes the hard horizon in reflections.

### Phase 5 — Ship it in recreation

20. **RCGI as recreation's quality-preset GI** (M, recreation) — wire `rcgi=true` into the
    high/ultra presets and the trailer RT mode, DDGI stays as the mid tier, SSGI low tier —
    mirroring their platform-scalability ladder (baked → RT diffuse → +RT specular). Close the
    known `RCGI.md` TODOs first (spec bounce reads DDGI atlas → read RCGI; cache-miss blend
    fallback black → sky/probe).
21. **Thin translucency at RT hits** (L, rx) — keep thin-blend materials (windows, banners) in
    the BLAS as non-opaque with a translucency factor in `MaterialRecord`; cache shade lights
    both faces; gather ray picks front/back with `t/(1+t)` probability and pdf rebalance, flip
    direction for SH encoding. Validate in a Skyrim interior with barred windows.
22. **Denoiser masks & vegetation disocclusion** (M, rx) — material-ID mask (vegetation /
    character / translucent) into the RCGI bilateral weights and NRD material ID; relax
    temporal reprojection + widen the search for wind-animated vegetation; extra Poisson
    spatial pass on disoccluded pixels.
23. **Quarter-res gather experiment** (S, rx) — flip the constant, tune the upscale, A/B on the
    golden harness; they shipped quarter-res diffuse on all consoles.

### Explicitly skipped, and why

- **Baked GI dual system, relightable local cubemaps** — their legacy/scalability burden; our
  low tier is SSGI/DDGI and the SDF software path already covers no-HW-RT.
- **Season LUTs, two-channel seasonal alpha** — recreation has no season system; revisit only
  if a seasons feature lands.
- **Terrain world-space atlas in the BVH** — our per-cell splat bake already serves as the
  simplified RT terrain material; a world atlas buys little at our terrain texel density.
- **Probe gbuffer caching (relight without retrace)** — their probes retrace rarely (~2 s
  cadence) so caching gbuffers pays off; RCGI's hash cache already amortizes hits across
  probes and pixels and re-shades cheaply. Reconsider only if probe-trace cost becomes the
  bottleneck.
- **2D→3D motion vectors** — flagged in their limitations; our TAA/NRD path already consumes
  depth-aware reprojection.

## 4. Sequencing & verification

- Phases 1 and 2 are independent and can proceed in parallel branches off rx `main`
  (`feature/rt-vegetation`, `feature/rt-scene-scaling`). Phase 3 item 9 needs a small
  recreation-side PR (interior volume forwarding) and should land after the in-flight
  `feature/streamed-props` merge to avoid `cell_streaming.cc` churn.
- Every phase gates on: golden-image harness diff (raster parity), `RX_RCGI=1` +
  `RX_PATHTRACE_REFERENCE` A/B for energy correctness, and real-GPU verification (vkrun /
  NVIDIA — lavapipe only as a smoke test), plus a Skyrim interior + dense-forest exterior
  capture pair as the standing test scenes.
- Rough total: ~8-10 engineer-weeks across both repos, front-loaded so that after Phases 1+3
  the visible wins (vegetation GI, leak-free interiors) are already shippable.
