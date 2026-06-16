recreation_add_module(render
  renderer.cc
  render_graph.cc
  mesh_pipeline.cc
  material_system.cc
  bindless.cc
  environment.cc
  ambient_occlusion.cc
  bloom.cc
  ddgi.cc
  exposure.cc
  post.cc
  shader_util.cc
  taa.cc
  raytracing.cc
  upscaler.cc
  vulkan/vk_device.cc
  vulkan/vk_swapchain.cc
  vulkan/vk_resources.cc
  vulkan/vma_impl.cc
)

target_link_libraries(recreation_render
  PUBLIC recreation::core recreation::asset volk GPUOpen::VulkanMemoryAllocator)

if(RECREATION_FSR3)
  target_sources(recreation_render PRIVATE upscaler_fsr3.cc)
  target_compile_definitions(recreation_render PRIVATE RECREATION_HAS_FSR3)
  target_link_libraries(recreation_render PRIVATE recreation::ffx_fsr3)
endif()

if(NOT MSVC)
  set_source_files_properties(vulkan/vma_impl.cc PROPERTIES COMPILE_OPTIONS -w)
endif()

recreation_embed_shaders(recreation_render
  shaders/mesh.vs.hlsl
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
  shaders/rtao_blur.cs.hlsl
  shaders/bloom_down.cs.hlsl
  shaders/bloom_up.cs.hlsl
  shaders/histogram.cs.hlsl
  shaders/exposure_resolve.cs.hlsl
  shaders/ddgi_rays.cs.hlsl
  shaders/ddgi_blend.cs.hlsl
  shaders/ddgi_border.cs.hlsl
)
