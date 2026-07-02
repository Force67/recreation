recreation_add_module(render
  core/renderer.cc
  core/presets.cc
  core/settings_ini.cc
  core/render_graph.cc
  core/bindless.cc
  pipeline/mesh_pipeline.cc
  pipeline/material_system.cc
  pipeline/meshlet.cc
  pipeline/gpu_cull.cc
  gi/shadow.cc
  gi/shadow_trace.cc
  gi/ddgi.cc
  gi/raytracing.cc
  gi/path_tracer.cc
  gi/recon_path_tracer.cc
  screenspace/ambient_occlusion.cc
  screenspace/ssao.cc
  screenspace/ssr.cc
  screenspace/ssgi.cc
  atmosphere/environment.cc
  atmosphere/volumetric_fog.cc
  atmosphere/aerial_perspective.cc
  atmosphere/clouds.cc
  atmosphere/precipitation.cc
  atmosphere/surface_weather.cc
  post/post.cc
  post/bloom.cc
  post/exposure.cc
  post/taa.cc
  post/overdraw.cc
  post/ui_blur.cc
  post/upscaler.cc
  geometry/water.cc
  geometry/particles.cc
  geometry/fur.cc
  geometry/wboit.cc
  geometry/gaussian.cc
  util/exr_write.cc
  util/gpu_profiler.cc
  util/stb_impl.cc
  rhi/rhi.cc
  null/null_device.cc
)

# Graphics backends. Vulkan is the default everywhere it can run; d3d12 is the
# native windows/xbox path, and on Linux it runs natively over vkd3d
# (D3D12-on-Vulkan) for cross-backend validation. The null backend is always
# built.
option(RECREATION_RHI_VULKAN "Build the Vulkan render backend" ON)
if(WIN32)
  option(RECREATION_RHI_D3D12 "Build the D3D12 render backend" ON)
else()
  # Default to ON when vkd3d is available (the nix dev shell provides it).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(VKD3D_PROBE QUIET libvkd3d libvkd3d-utils)
  endif()
  if(VKD3D_PROBE_FOUND)
    option(RECREATION_RHI_D3D12 "Build the D3D12 render backend" ON)
  else()
    option(RECREATION_RHI_D3D12 "Build the D3D12 render backend" OFF)
  endif()
endif()

if(RECREATION_RHI_VULKAN)
  target_sources(recreation_render PRIVATE
    vulkan/vk_device.cc
    vulkan/vk_swapchain.cc
    vulkan/vk_command_list.cc
    vulkan/vk_convert.cc
    vulkan/vk_interop.cc
    vulkan/vma_impl.cc
    util/shader_util.cc
  )
  # PUBLIC: interop consumers (gui backend, thumbnailer, upscalers) gate their
  # Vulkan paths on this define.
  target_compile_definitions(recreation_render PUBLIC RECREATION_RHI_VULKAN)
endif()

if(RECREATION_RHI_D3D12)
  target_sources(recreation_render PRIVATE
    d3d12/d3d12_device.cc
    d3d12/d3d12_command_list.cc
    d3d12/d3d12_swapchain.cc
    d3d12/d3d12_convert.cc
  )
  # PUBLIC: REC_SHADER grows the DXIL sidecar reference under this define, so
  # every consumer of the embed headers must agree.
  target_compile_definitions(recreation_render PUBLIC RECREATION_RHI_D3D12)
  if(WIN32)
    target_link_libraries(recreation_render PRIVATE d3d12 dxgi)
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VKD3D REQUIRED IMPORTED_TARGET libvkd3d libvkd3d-utils)
    target_link_libraries(recreation_render PRIVATE PkgConfig::VKD3D)
  endif()
endif()

target_link_libraries(recreation_render
  PUBLIC recreation::core recreation::asset volk GPUOpen::VulkanMemoryAllocator)
target_link_libraries(recreation_render PRIVATE recreation::stb)

if(RECREATION_FSR3)
  target_sources(recreation_render PRIVATE post/upscaler_fsr3.cc)
  target_compile_definitions(recreation_render PRIVATE RECREATION_HAS_FSR3)
  target_link_libraries(recreation_render PRIVATE recreation::ffx_fsr3)
endif()

if(RECREATION_DLSS)
  target_sources(recreation_render PRIVATE
    post/upscaler_dlss.cc
    post/ngx_context.cc
    gi/denoiser_rr.cc)
  # PUBLIC: renderer.h embeds an RrDenoiser member under this macro, so every
  # translation unit that includes it must agree on the layout (same rule as
  # RECREATION_HAS_NRD below).
  target_compile_definitions(recreation_render PUBLIC RECREATION_HAS_DLSS)
  target_link_libraries(recreation_render PRIVATE recreation::dlss)
endif()

if(RECREATION_NRD)
  target_sources(recreation_render PRIVATE gi/denoiser_nrd.cc)
  # PUBLIC: renderer.h embeds an NrdDenoiser member under this macro, so every
  # translation unit that includes it must agree on the layout or the Renderer
  # ctor writes past a shorter allocation.
  target_compile_definitions(recreation_render PUBLIC RECREATION_HAS_NRD)
  target_link_libraries(recreation_render PRIVATE recreation::nrd)
endif()

if(NOT MSVC)
  set_source_files_properties(vulkan/vma_impl.cc util/stb_impl.cc PROPERTIES COMPILE_OPTIONS -w)
endif()

set(RECREATION_RENDER_SHADERS
  shaders/pipeline/mesh.vs.hlsl
  shaders/pipeline/mesh_skin.vs.hlsl
  shaders/gi/shadow.vs.hlsl
  shaders/gi/shadow_skin.vs.hlsl
  shaders/gi/shadow.ps.hlsl
  shaders/geometry/particle.vs.hlsl
  shaders/geometry/particle.ps.hlsl
  shaders/geometry/particle_sim.cs.hlsl
  shaders/geometry/fur.vs.hlsl
  shaders/geometry/fur.ps.hlsl
  shaders/geometry/wboit.vs.hlsl
  shaders/geometry/wboit.ps.hlsl
  shaders/geometry/wboit_resolve.ps.hlsl
  shaders/geometry/gsplat.vs.hlsl
  shaders/geometry/gsplat.ps.hlsl
  shaders/post/overdraw.ps.hlsl
  shaders/pipeline/mesh.ps.hlsl
  shaders/pipeline/mesh_rt.ps.hlsl
  shaders/util/fullscreen.vs.hlsl
  shaders/util/blit.ps.hlsl
  shaders/post/tonemap.ps.hlsl
  shaders/post/taa.cs.hlsl
  shaders/atmosphere/transmittance_lut.cs.hlsl
  shaders/atmosphere/multiscatter_lut.cs.hlsl
  shaders/atmosphere/aerial_perspective.cs.hlsl
  shaders/atmosphere/clouds.cs.hlsl
  shaders/atmosphere/precipitation.cs.hlsl
  shaders/atmosphere/surface_weather.cs.hlsl
  shaders/atmosphere/sky.cs.hlsl
  shaders/atmosphere/sky.ps.hlsl
  shaders/atmosphere/irradiance.cs.hlsl
  shaders/atmosphere/prefilter.cs.hlsl
  shaders/atmosphere/brdf_lut.cs.hlsl
  shaders/pipeline/prepass.ps.hlsl
  shaders/screenspace/rtao.cs.hlsl
  shaders/screenspace/ssao.cs.hlsl
  shaders/screenspace/ssr.cs.hlsl
  shaders/screenspace/ssgi.cs.hlsl
  shaders/pipeline/cull.cs.hlsl
  shaders/pipeline/hiz_reduce.cs.hlsl
  shaders/pipeline/meshlet.ms.hlsl
  shaders/pipeline/meshlet.ps.hlsl
  shaders/pipeline/mesh_scene.as.hlsl
  shaders/pipeline/mesh_scene.ms.hlsl
  shaders/pipeline/bounds.vs.hlsl
  shaders/pipeline/bounds.ps.hlsl
  shaders/post/bloom_down.cs.hlsl
  shaders/post/bloom_up.cs.hlsl
  shaders/post/histogram.cs.hlsl
  shaders/post/exposure_resolve.cs.hlsl
  shaders/gi/ddgi_rays.cs.hlsl
  shaders/gi/ddgi_blend.cs.hlsl
  shaders/gi/ddgi_border.cs.hlsl
  shaders/geometry/water.ps.hlsl
  shaders/geometry/copy.cs.hlsl
  shaders/util/hdr_capture.cs.hlsl
  shaders/gi/pathtrace.cs.hlsl
  shaders/gi/recon_gbuffer.cs.hlsl
  shaders/gi/recon_restir_temporal.cs.hlsl
  shaders/gi/recon_restir_spatial.cs.hlsl
  shaders/gi/recon_temporal.cs.hlsl
  shaders/gi/recon_atrous.cs.hlsl
  shaders/gi/recon_composite.cs.hlsl
  shaders/atmosphere/fog.cs.hlsl
  shaders/post/ui_blur.ps.hlsl
)

# rtao.cs.hlsl and the nrd_* shaders pull in NRD.hlsli. The include dir is added
# whenever NRD is vendored (independent of RECREATION_NRD, so an NRD-off build of
# a vendored tree still compiles rtao). The denoiser-only inputs are embedded
# only when the denoiser is built.
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/NRD/Shaders/NRD.hlsli)
  set(RECREATION_SHADER_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/NRD/Shaders)
endif()
# Shaders include siblings from their own subfolder (atmosphere.hlsli; the
# mesh/shadow base vertex shaders reused by the *_skin variants). Add those dirs
# so dxc resolves the quoted includes regardless of the including file's path.
list(APPEND RECREATION_SHADER_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/atmosphere
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pipeline
  ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gi)
if(RECREATION_NRD)
  list(APPEND RECREATION_RENDER_SHADERS shaders/gi/nrd_pack.cs.hlsl shaders/gi/shadow_trace.cs.hlsl
       shaders/gi/pathtrace_gbuffer.cs.hlsl shaders/gi/pathtrace_composite.cs.hlsl)
endif()

# SPIR-V-only shaders: everything reading GPU memory through buffer device
# addresses (vk::RawBufferLoad), which has no DXIL equivalent. The meshlet
# path and the ray-hit shading readers need a descriptor-based port before
# they can run on d3d12 (see RHI.md); the d3d12 device reports their pipelines
# unavailable and the engine's caps/feature gating keeps them unreached (mesh
# shaders and ray query are off under vkd3d). The skinned vertex permutations
# are NOT here: they carry a dual path (rec_bone_palette root SRV on DXIL).
set(RECREATION_SHADER_NO_DXIL
  mesh_scene.as.hlsl
  mesh_scene.ms.hlsl
  meshlet.ms.hlsl
  mesh_rt.ps.hlsl
  water.ps.hlsl
  pathtrace.cs.hlsl
  pathtrace_gbuffer.cs.hlsl
  ddgi_rays.cs.hlsl
  recon_gbuffer.cs.hlsl
)

recreation_embed_shaders(recreation_render ${RECREATION_RENDER_SHADERS})
