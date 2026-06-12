# HLSL compiled to spirv with dxc at build time and embedded as C arrays.
# The stage comes from the file name: <name>.vs.hlsl, <name>.ps.hlsl or
# <name>.cs.hlsl. Symbols follow MAKE_C_IDENTIFIER, e.g. mesh.vs.hlsl embeds
# as k_mesh_vs_hlsl in generated/shaders/mesh_vs_hlsl.h.
function(recreation_embed_shaders target)
  set(headers)
  foreach(shader ${ARGN})
    get_filename_component(name ${shader} NAME)
    string(MAKE_C_IDENTIFIER ${name} symbol)
    if(name MATCHES "\\.vs\\.hlsl$")
      set(profile vs_6_6)
    elseif(name MATCHES "\\.ps\\.hlsl$")
      set(profile ps_6_6)
    elseif(name MATCHES "\\.cs\\.hlsl$")
      set(profile cs_6_6)
    else()
      message(FATAL_ERROR "cannot derive a shader stage from ${name}")
    endif()
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
    set(header ${CMAKE_BINARY_DIR}/generated/shaders/${symbol}.h)
    add_custom_command(OUTPUT ${spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
      COMMAND ${RECREATION_DXC} -spirv -fspv-target-env=vulkan1.3 -T ${profile} -E main
              -Fo ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
      COMMENT "hlsl ${name}")
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DHEADER=${header} -DSYMBOL=${symbol}
              -P ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      DEPENDS ${spv} ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      COMMENT "embed ${name}")
    list(APPEND headers ${header})
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${headers})
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
