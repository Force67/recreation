recreation_add_module(render
  renderer.cc
  render_graph.cc
  mesh_pipeline.cc
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
)
