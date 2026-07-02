# HLSL compiled to spirv with dxc at build time and embedded as C arrays.
# The stage comes from the file name: <name>.vs.hlsl, <name>.ps.hlsl or
# <name>.cs.hlsl. Symbols follow MAKE_C_IDENTIFIER, e.g. mesh.vs.hlsl embeds
# as k_mesh_vs_hlsl in generated/shaders/mesh_vs_hlsl.h.
#
# When the d3d12 backend is enabled (RECREATION_RHI_D3D12) every shader also
# gets a DXIL sidecar (same dxc invocation minus -spirv) embedded as
# k_<symbol>_dxil. The DXIL target is SM 6.5, not 6.6: it is the highest model
# accepted by vkd3d 2.0 (the Linux D3D12 layer used for validation) and still
# covers ray queries (6.5) and mesh shaders (6.5). The DXIL is unsigned, which
# vkd3d accepts natively and Windows accepts with experimental shader models
# enabled; production Windows builds would sign via dxil.dll.
function(recreation_embed_shaders target)
  # Optional -I include dirs for shaders that pull in vendored headers (e.g.
  # NRD.hlsli), passed via the RECREATION_SHADER_INCLUDE_DIRS list variable.
  set(include_flags)
  foreach(dir ${RECREATION_SHADER_INCLUDE_DIRS})
    list(APPEND include_flags -I ${dir})
  endforeach()
  set(headers)
  foreach(shader ${ARGN})
    get_filename_component(name ${shader} NAME)
    string(MAKE_C_IDENTIFIER ${name} symbol)
    if(name MATCHES "\\.vs\\.hlsl$")
      set(stage vs)
    elseif(name MATCHES "\\.ps\\.hlsl$")
      set(stage ps)
    elseif(name MATCHES "\\.cs\\.hlsl$")
      set(stage cs)
    elseif(name MATCHES "\\.ms\\.hlsl$")
      set(stage ms)
    elseif(name MATCHES "\\.as\\.hlsl$")
      set(stage as)
    else()
      message(FATAL_ERROR "cannot derive a shader stage from ${name}")
    endif()
    set(profile ${stage}_6_6)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
    set(header ${CMAKE_BINARY_DIR}/generated/shaders/${symbol}.h)
    add_custom_command(OUTPUT ${spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
      COMMAND ${RECREATION_DXC} -spirv -fspv-target-env=vulkan1.3 -T ${profile} -E main
              ${include_flags} -Fo ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
      COMMENT "hlsl ${name}")
    set(embed_args)
    set(embed_deps ${spv})
    if(RECREATION_RHI_D3D12)
      set(dxil_profile ${stage}_6_5)
      set(dxil ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.dxil)
      add_custom_command(OUTPUT ${dxil}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${RECREATION_DXC} -T ${dxil_profile} -E main -Qstrip_reflect
                ${include_flags} -Fo ${dxil} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
        COMMENT "dxil ${name}")
      list(APPEND embed_args -DDXIL=${dxil})
      list(APPEND embed_deps ${dxil})
    endif()
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DHEADER=${header} -DSYMBOL=${symbol}
              ${embed_args} -P ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      DEPENDS ${embed_deps} ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      COMMENT "embed ${name}")
    list(APPEND headers ${header})
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${headers})
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endfunction()
