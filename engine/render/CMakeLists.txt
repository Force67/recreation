recreation_add_module(render
  renderer.cc
  presets.cc
  render_graph.cc
  mesh_pipeline.cc
  material_system.cc
  bindless.cc
  water.cc
  stb_impl.cc
  environment.cc
  ambient_occlusion.cc
  ssao.cc
  gpu_cull.cc
  shadow.cc
  particles.cc
  gaussian.cc
  overdraw.cc
  bloom.cc
  ddgi.cc
  exposure.cc
  post.cc
  shader_util.cc
  taa.cc
  raytracing.cc
  gpu_profiler.cc
  path_tracer.cc
  volumetric_fog.cc
  upscaler.cc
  vulkan/vk_device.cc
  vulkan/vk_swapchain.cc
  vulkan/vk_resources.cc
  vulkan/vma_impl.cc
)

target_link_libraries(recreation_render
  PUBLIC recreation::core recreation::asset volk GPUOpen::VulkanMemoryAllocator)
target_link_libraries(recreation_render PRIVATE recreation::stb)

if(RECREATION_FSR3)
  target_sources(recreation_render PRIVATE upscaler_fsr3.cc)
  target_compile_definitions(recreation_render PRIVATE RECREATION_HAS_FSR3)
  target_link_libraries(recreation_render PRIVATE recreation::ffx_fsr3)
endif()

if(RECREATION_DLSS)
  target_sources(recreation_render PRIVATE upscaler_dlss.cc)
  target_compile_definitions(recreation_render PRIVATE RECREATION_HAS_DLSS)
  target_link_libraries(recreation_render PRIVATE recreation::dlss)
endif()

if(RECREATION_NRD)
  target_sources(recreation_render PRIVATE denoiser_nrd.cc)
  # PUBLIC: renderer.h embeds an NrdDenoiser member under this macro, so every
  # translation unit that includes it must agree on the layout or the Renderer
  # ctor writes past a shorter allocation.
  target_compile_definitions(recreation_render PUBLIC RECREATION_HAS_NRD)
  target_link_libraries(recreation_render PRIVATE recreation::nrd)
endif()

if(NOT MSVC)
  set_source_files_properties(vulkan/vma_impl.cc stb_impl.cc PROPERTIES COMPILE_OPTIONS -w)
endif()

set(RECREATION_RENDER_SHADERS
  shaders/mesh.vs.hlsl
  shaders/mesh_skin.vs.hlsl
  shaders/shadow.vs.hlsl
  shaders/shadow.ps.hlsl
  shaders/particle.vs.hlsl
  shaders/particle.ps.hlsl
  shaders/gsplat.vs.hlsl
  shaders/gsplat.ps.hlsl
  shaders/overdraw.ps.hlsl
  shaders/mesh.ps.hlsl
  shaders/mesh_rt.ps.hlsl
  shaders/fullscreen.vs.hlsl
  shaders/tonemap.ps.hlsl
  shaders/taa.cs.hlsl
  shaders/sky.cs.hlsl
  shaders/sky.ps.hlsl
  shaders/irradiance.cs.hlsl
  shaders/prefilter.cs.hlsl
  shaders/brdf_lut.cs.hlsl
  shaders/prepass.ps.hlsl
  shaders/rtao.cs.hlsl
  shaders/ssao.cs.hlsl
  shaders/cull.cs.hlsl
  shaders/bounds.vs.hlsl
  shaders/bounds.ps.hlsl
  shaders/bloom_down.cs.hlsl
  shaders/bloom_up.cs.hlsl
  shaders/histogram.cs.hlsl
  shaders/exposure_resolve.cs.hlsl
  shaders/ddgi_rays.cs.hlsl
  shaders/ddgi_blend.cs.hlsl
  shaders/ddgi_border.cs.hlsl
  shaders/water.ps.hlsl
  shaders/copy.cs.hlsl
  shaders/pathtrace.cs.hlsl
  shaders/fog.cs.hlsl
)

# rtao.cs.hlsl and the nrd_* shaders pull in NRD.hlsli. The include dir is added
# whenever NRD is vendored (independent of RECREATION_NRD, so an NRD-off build of
# a vendored tree still compiles rtao). The denoiser-only inputs are embedded
# only when the denoiser is built.
if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/NRD/Shaders/NRD.hlsli)
  set(RECREATION_SHADER_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/NRD/Shaders)
endif()
if(RECREATION_NRD)
  list(APPEND RECREATION_RENDER_SHADERS shaders/nrd_pack.cs.hlsl shaders/shadow_trace.cs.hlsl)
endif()

recreation_embed_shaders(recreation_render ${RECREATION_RENDER_SHADERS})
