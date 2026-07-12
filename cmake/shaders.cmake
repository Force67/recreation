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
  # Compiled blobs are also aggregated into ${CMAKE_BINARY_DIR}/shaders.rxp so
  # they can be shipped in (and loaded from) an rx game archive instead of the
  # binary. Each shader is staged into pack_dir under a clean virtual name
  # (<stem>.spv, plus <stem>.dxil on the d3d12 backend) and rxpack packs the
  # tree. The embedded C arrays above stay the runtime fallback.
  set(pack_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}_shader_pack)
  set(pack_staged)
  set(headers)
  foreach(shader ${ARGN})
    get_filename_component(name ${shader} NAME)
    string(MAKE_C_IDENTIFIER ${name} symbol)
    # Virtual stem: source name with the trailing .hlsl dropped (ugui_quad.vs).
    string(REGEX REPLACE "\\.hlsl$" "" stem ${name})
    set(staged_dxil)
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
    # #include dependencies are not tracked automatically; wrapper shaders
    # (variant defines around a shared body) list their includes via
    # RECREATION_SHADER_DEPS_<symbol> so edits to the body rebuild the variant.
    set(extra_deps)
    if(DEFINED RECREATION_SHADER_DEPS_${symbol})
      foreach(dep ${RECREATION_SHADER_DEPS_${symbol}})
        list(APPEND extra_deps ${CMAKE_CURRENT_SOURCE_DIR}/${dep})
      endforeach()
    endif()
    add_custom_command(OUTPUT ${spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
      COMMAND ${RECREATION_DXC} -spirv -fspv-target-env=vulkan1.3 -T ${profile} -E main
              ${include_flags} -Fo ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader} ${extra_deps}
      COMMENT "hlsl ${name}")
    set(embed_args)
    set(embed_deps ${spv})
    # Shaders on the RECREATION_SHADER_NO_DXIL list cannot target DXIL (they
    # use SPIR-V-only constructs, chiefly vk::RawBufferLoad buffer-device-
    # address reads). Their sidecar embeds as a null pointer, which the d3d12
    # device reports as "pipeline unavailable" instead of failing the build.
    if(RECREATION_RHI_D3D12 AND name IN_LIST RECREATION_SHADER_NO_DXIL)
      list(APPEND embed_args -DDXIL_MISSING=1)
    elseif(RECREATION_RHI_D3D12)
      set(dxil_profile ${stage}_6_5)
      set(dxil ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.dxil)
      add_custom_command(OUTPUT ${dxil}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
        COMMAND ${RECREATION_DXC} -T ${dxil_profile} -E main -Qstrip_reflect
                ${include_flags} -Fo ${dxil} ${CMAKE_CURRENT_SOURCE_DIR}/${shader}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${shader} ${extra_deps}
        COMMENT "dxil ${name}")
      list(APPEND embed_args -DDXIL=${dxil})
      list(APPEND embed_deps ${dxil})
      set(staged_dxil ${dxil})
    endif()
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND} -DSPV=${spv} -DHEADER=${header} -DSYMBOL=${symbol}
              ${embed_args} -P ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      DEPENDS ${embed_deps} ${CMAKE_SOURCE_DIR}/cmake/embed_spv.cmake
      COMMENT "embed ${name}")
    list(APPEND headers ${header})

    # Stage the compiled blobs under clean virtual names for the archive.
    set(staged_spv ${pack_dir}/${stem}.spv)
    add_custom_command(OUTPUT ${staged_spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${pack_dir}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${spv} ${staged_spv}
      DEPENDS ${spv}
      COMMENT "stage ${stem}.spv")
    list(APPEND pack_staged ${staged_spv})
    if(staged_dxil)
      set(staged_dxil_out ${pack_dir}/${stem}.dxil)
      add_custom_command(OUTPUT ${staged_dxil_out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${pack_dir}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${staged_dxil} ${staged_dxil_out}
        DEPENDS ${staged_dxil}
        COMMENT "stage ${stem}.dxil")
      list(APPEND pack_staged ${staged_dxil_out})
    endif()
  endforeach()
  add_custom_target(${target}_shaders DEPENDS ${headers})
  add_dependencies(${target} ${target}_shaders)
  target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)

  # Pack the staged blobs into shaders.rxp with the rxpack tool (an rx target
  # brought in via add_subdirectory). The archive rebuilds whenever any staged
  # blob changes; the game mounts it under the shaders:// scheme at startup.
  set(pack_archive ${CMAKE_BINARY_DIR}/shaders.rxp)
  add_custom_command(OUTPUT ${pack_archive}
    COMMAND $<TARGET_FILE:rxpack> create ${pack_archive} ${pack_dir}
    DEPENDS ${pack_staged} rxpack
    COMMENT "pack ${target} shaders -> shaders.rxp")
  add_custom_target(${target}_shader_pack DEPENDS ${pack_archive})
  add_dependencies(${target} ${target}_shader_pack)
endfunction()
