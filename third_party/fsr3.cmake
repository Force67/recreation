# AMD FidelityFX FSR 3.1 upscaler, vendor-compiled from the SDK checkout in
# third_party/FidelityFX-SDK (see tools/get_fidelityfx.sh). The SDK's own
# CMake is Windows-only, so the needed sources are built directly:
#   - ffx_sc: the FidelityFX shader compiler host tool (GLSL path only),
#     built with small Windows API shims from ffx_shim/sc/.
#   - shader permutation headers: generated at build time by driving ffx_sc
#     over the 10 fsr3upscaler passes (4 variants each), which shells out to
#     glslangValidator from the dev shell.
#   - recreation_ffx_fsr3: the classic components API (fsr3upscaler host
#     component + vulkan backend + shader blob accessors) as a static lib.

set(FFX_SDK ${CMAKE_CURRENT_SOURCE_DIR}/FidelityFX-SDK/sdk)
set(FFX_SHIM ${CMAKE_CURRENT_SOURCE_DIR}/ffx_shim)
set(FFX_SC_DIR ${FFX_SDK}/tools/ffx_shader_compiler)
set(FFX_FSR3_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/ffx_shaders/vk)

find_program(RECREATION_GLSLANG glslangValidator REQUIRED)
find_package(Threads REQUIRED)
enable_language(C)  # the tool's vendored SPIRV-Reflect is C

# ---------------------------------------------------------------------------
# Host shader compiler tool.
add_executable(ffx_sc
  ${FFX_SC_DIR}/src/ffx_sc.cpp
  ${FFX_SC_DIR}/src/glsl_compiler.cpp
  ${FFX_SC_DIR}/src/utils.cpp
  ${FFX_SC_DIR}/libs/MD5/md5.cpp
  ${FFX_SC_DIR}/libs/SPIRV-Reflect/spirv_reflect.c
  ${FFX_SC_DIR}/libs/tiny-process-library/process.cpp
  ${FFX_SHIM}/sc/hlsl_compiler_stub.cc
)
target_include_directories(ffx_sc PRIVATE
  ${FFX_SC_DIR}/src
  ${FFX_SC_DIR}/libs/MD5
  ${FFX_SC_DIR}/libs/SPIRV-Reflect
  ${FFX_SC_DIR}/libs/tiny-process-library
)
if(WIN32)
  # Native Windows build: the real SDK supplies <Windows.h>, <d3dcompiler.h>,
  # <d3d12shader.h> and <pathcch.h>; sc_win only adds the two headers it lacks
  # (a forward-declared DXC type set and a trivial CComPtr) so the stubbed HLSL
  # path parses. Process spawning uses the Win32 backend; wmain() is native.
  target_sources(ffx_sc PRIVATE ${FFX_SC_DIR}/libs/tiny-process-library/process_win.cpp)
  target_include_directories(ffx_sc BEFORE PRIVATE ${FFX_SHIM}/sc_win)
  target_link_libraries(ffx_sc PRIVATE Pathcch)
else()
  # Non-Windows: the Linux shim fakes the whole Win32 surface ffx_sc needs
  # (Windows.h, pathcch, DXC headers, the main()->wmain() bridge) so the
  # unmodified SDK sources build with glslang only.
  target_sources(ffx_sc PRIVATE
    ${FFX_SC_DIR}/libs/tiny-process-library/process_unix.cpp
    ${FFX_SHIM}/sc/win_shim.cc)
  target_include_directories(ffx_sc BEFORE PRIVATE ${FFX_SHIM}/sc)
endif()
target_compile_options(ffx_sc PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
if(NOT MSVC)
  # The tool's MD5 hex printer calls snprintf with an overstated buffer size,
  # which a toolchain defaulting _FORTIFY_SOURCE on (nix at =3, the Ubuntu CI
  # image at =2) turns into an abort at runtime. The nix wrapper appends its
  # hardening flags after ours, so the env switch is the only reliable off knob
  # there; the Ubuntu default yields to a command-line -D_FORTIFY_SOURCE=0. Both
  # are set; each is a no-op on the other's toolchain. Keep pic/pie so linking
  # still works under nix.
  set_target_properties(ffx_sc PROPERTIES
    C_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;NIX_HARDENING_ENABLE=pic pie"
    CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env;NIX_HARDENING_ENABLE=pic pie")
  target_compile_options(ffx_sc PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
endif()
target_link_libraries(ffx_sc PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# Shader permutation headers. ffx_sc expands the {0,1} permutation sets
# itself (one invocation per pass and variant); the invocation lives in
# cmake/ffx_sc_compile.cmake so the braces never meet a shell. No depfiles:
# the SDK checkout is treated as immutable.
set(FFX_FSR3_PASSES
  accumulate
  autogen_reactive
  debug_view
  luma_instability
  luma_pyramid
  prepare_inputs
  prepare_reactivity
  rcas
  shading_change
  shading_change_pyramid
)

file(MAKE_DIRECTORY ${FFX_FSR3_SHADER_DIR})
set(FFX_FSR3_PERMUTATION_HEADERS)
foreach(pass IN LISTS FFX_FSR3_PASSES)
  set(shader ${FFX_SDK}/src/backends/vk/shaders/fsr3upscaler/ffx_fsr3upscaler_${pass}_pass.glsl)
  foreach(variant base wave64 16bit wave64_16bit)
    if(variant STREQUAL "base")
      set(suffix "")
      set(half 0)
    elseif(variant STREQUAL "wave64")
      set(suffix "_wave64")
      set(half 0)
    elseif(variant STREQUAL "16bit")
      set(suffix "_16bit")
      set(half 1)
    else()
      set(suffix "_wave64_16bit")
      set(half 1)
    endif()
    set(name ffx_fsr3upscaler_${pass}_pass${suffix})
    set(header ${FFX_FSR3_SHADER_DIR}/${name}_permutations.h)
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND}
              -DFFX_SC=$<TARGET_FILE:ffx_sc>
              -DGLSLANG=${RECREATION_GLSLANG}
              -DGPU_DIR=${FFX_SDK}/include/FidelityFX/gpu
              -DOUT_DIR=${FFX_FSR3_SHADER_DIR}
              -DNAME=${name}
              -DHALF=${half}
              -DSHADER=${shader}
              -P ${CMAKE_SOURCE_DIR}/cmake/ffx_sc_compile.cmake
      DEPENDS ffx_sc ${shader} ${CMAKE_SOURCE_DIR}/cmake/ffx_sc_compile.cmake
      COMMENT "ffx_sc ${name}"
      VERBATIM)
    list(APPEND FFX_FSR3_PERMUTATION_HEADERS ${header})
  endforeach()
endforeach()

# ---------------------------------------------------------------------------
# Frame generation permutations: the opticalflow + frameinterpolation passes,
# same four variants as the upscaler but with each effect's own option set
# (cmake/ffx_sc_compile_fg.cmake mirrors the SDK's per-effect args).
set(FFX_FG_SHADERS
  opticalflow/ffx_opticalflow_prepare_luma_pass
  opticalflow/ffx_opticalflow_compute_luminance_pyramid_pass
  opticalflow/ffx_opticalflow_generate_scd_histogram_pass
  opticalflow/ffx_opticalflow_compute_scd_divergence_pass
  opticalflow/ffx_opticalflow_compute_optical_flow_advanced_pass_v5
  opticalflow/ffx_opticalflow_filter_optical_flow_pass_v5
  opticalflow/ffx_opticalflow_scale_optical_flow_advanced_pass_v5
  frameinterpolation/ffx_frameinterpolation_setup_pass
  frameinterpolation/ffx_frameinterpolation_reconstruct_previous_depth_pass
  frameinterpolation/ffx_frameinterpolation_reconstruct_and_dilate_pass
  frameinterpolation/ffx_frameinterpolation_game_motion_vector_field_pass
  frameinterpolation/ffx_frameinterpolation_optical_flow_vector_field_pass
  frameinterpolation/ffx_frameinterpolation_disocclusion_mask_pass
  frameinterpolation/ffx_frameinterpolation_pass
  frameinterpolation/ffx_frameinterpolation_compute_game_vector_field_inpainting_pyramid_pass
  frameinterpolation/ffx_frameinterpolation_compute_inpainting_pyramid_pass
  frameinterpolation/ffx_frameinterpolation_inpainting_pass
  frameinterpolation/ffx_frameinterpolation_debug_view_pass
)
foreach(entry IN LISTS FFX_FG_SHADERS)
  get_filename_component(shader_base ${entry} NAME)
  get_filename_component(effect ${entry} DIRECTORY)
  set(shader ${FFX_SDK}/src/backends/vk/shaders/${entry}.glsl)
  foreach(variant base wave64 16bit wave64_16bit)
    if(variant STREQUAL "base")
      set(suffix "")
      set(half 0)
    elseif(variant STREQUAL "wave64")
      set(suffix "_wave64")
      set(half 0)
    elseif(variant STREQUAL "16bit")
      set(suffix "_16bit")
      set(half 1)
    else()
      set(suffix "_wave64_16bit")
      set(half 1)
    endif()
    set(name ${shader_base}${suffix})
    set(header ${FFX_FSR3_SHADER_DIR}/${name}_permutations.h)
    add_custom_command(OUTPUT ${header}
      COMMAND ${CMAKE_COMMAND}
              -DFFX_SC=$<TARGET_FILE:ffx_sc>
              -DGLSLANG=${RECREATION_GLSLANG}
              -DGPU_DIR=${FFX_SDK}/include/FidelityFX/gpu
              -DOUT_DIR=${FFX_FSR3_SHADER_DIR}
              -DNAME=${name}
              -DHALF=${half}
              -DSHADER=${shader}
              -DEFFECT=${effect}
              -P ${CMAKE_SOURCE_DIR}/cmake/ffx_sc_compile_fg.cmake
      DEPENDS ffx_sc ${shader} ${CMAKE_SOURCE_DIR}/cmake/ffx_sc_compile_fg.cmake
      COMMENT "ffx_sc ${name}"
      VERBATIM)
    list(APPEND FFX_FSR3_PERMUTATION_HEADERS ${header})
  endforeach()
endforeach()

add_custom_target(ffx_fsr3_shaders DEPENDS ${FFX_FSR3_PERMUTATION_HEADERS})

# ---------------------------------------------------------------------------
# Runtime library: fsr3upscaler component + vulkan backend. ffx_compat.h is
# force-included for the MSVC string functions and to route the backend's
# vulkan calls through volk (the engine's loader).
add_library(recreation_ffx_fsr3 STATIC
  ${FFX_SDK}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
  ${FFX_SDK}/src/components/opticalflow/ffx_opticalflow.cpp
  ${FFX_SDK}/src/components/frameinterpolation/ffx_frameinterpolation.cpp
  ${FFX_SDK}/src/backends/vk/ffx_vk.cpp
  ${FFX_SDK}/src/backends/shared/ffx_shader_blobs.cpp
  ${FFX_SDK}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
  ${FFX_SDK}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
  ${FFX_SDK}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
  ${FFX_SDK}/src/shared/ffx_assert.cpp
  ${FFX_SDK}/src/shared/ffx_message.cpp
  ${FFX_SDK}/src/shared/ffx_object_management.cpp
  ${FFX_SDK}/src/shared/ffx_breadcrumbs_list.cpp
  ${FFX_SHIM}/ffx_fi_stub.cc
)
add_library(recreation::ffx_fsr3 ALIAS recreation_ffx_fsr3)
add_dependencies(recreation_ffx_fsr3 ffx_fsr3_shaders)
target_include_directories(recreation_ffx_fsr3
  PUBLIC
    ${FFX_SDK}/include
  PRIVATE
    ${FFX_SDK}/src
    ${FFX_SDK}/src/shared
    ${FFX_SDK}/src/components
    ${FFX_SDK}/src/backends/shared
    ${FFX_FSR3_SHADER_DIR}
)
if(NOT WIN32)
  # The interposed ffx_types.h bumps the SDK's default context size to cover the
  # 4-byte-wchar_t bloat, using #include_next (which MSVC lacks). Windows keeps
  # the 2-byte wchar_t the SDK sizes for, so the real header is used directly.
  target_include_directories(recreation_ffx_fsr3 BEFORE PUBLIC ${FFX_SHIM}/include)
endif()
target_compile_definitions(recreation_ffx_fsr3 PRIVATE FFX_FSR3UPSCALER FFX_OF FFX_FI)
if(MSVC)
  # /FI force-includes ffx_compat.h (volk routing); FFX_GCC stays undefined so
  # the SDK takes its native MSVC paths.
  target_compile_options(recreation_ffx_fsr3 PRIVATE /w /FI${FFX_SHIM}/ffx_compat.h)
else()
  target_compile_definitions(recreation_ffx_fsr3 PRIVATE FFX_GCC)
  target_compile_options(recreation_ffx_fsr3 PRIVATE -w -include ${FFX_SHIM}/ffx_compat.h)
endif()
# The SDK is C++17-era code; newer standards lose std::wstring_convert which
# its non-Windows string conversion path still uses.
set_target_properties(recreation_ffx_fsr3 PROPERTIES CXX_STANDARD 17)
target_link_libraries(recreation_ffx_fsr3 PUBLIC volk)
