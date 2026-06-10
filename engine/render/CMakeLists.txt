recreation_add_module(render
  renderer.cc
  render_graph.cc
  taa.cc
  raytracing.cc
  upscaler.cc
  vulkan/vk_device.cc
  vulkan/vk_swapchain.cc
)

target_link_libraries(recreation_render
  PUBLIC recreation::core recreation::asset recreation::ecs volk)
