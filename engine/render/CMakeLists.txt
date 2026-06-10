recreation_add_module(render
  renderer.cc
  render_graph.cc
  taa.cc
  raytracing.cc
  upscaler.cc
  vulkan/vk_device.cc
)

target_link_libraries(recreation_render PUBLIC recreation::core recreation::asset recreation::ecs)

if(Vulkan_FOUND)
  target_link_libraries(recreation_render PUBLIC Vulkan::Vulkan)
  target_compile_definitions(recreation_render PUBLIC RECREATION_HAS_VULKAN=1)
endif()
