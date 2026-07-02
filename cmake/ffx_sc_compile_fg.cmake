# Runs the FidelityFX shader compiler for one opticalflow / frameinterpolation
# pass variant. Argument sets mirror the SDK's per-effect compile lists
# (CMakeCompileOpticalflowShaders.txt / CMakeCompileFrameinterpolationShaders.txt,
# glslang/Vulkan path). Like ffx_sc_compile.cmake this runs as a -P script so
# the {0,1} permutation braces never meet a shell.
#
# Expects: FFX_SC, GLSLANG, GPU_DIR, OUT_DIR, NAME, HALF, SHADER, EFFECT

if(EFFECT STREQUAL "opticalflow")
  set(effect_args
    "-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}")
elseif(EFFECT STREQUAL "frameinterpolation")
  set(effect_args
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1
    -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
    -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
    "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}"
    "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}"
    "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}")
else()
  message(FATAL_ERROR "unknown ffx effect '${EFFECT}'")
endif()

execute_process(
  COMMAND ${FFX_SC}
    -reflection
    -DFFX_GPU=1
    -compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1
    ${effect_args}
    -glslangexe=${GLSLANG}
    -I${GPU_DIR}
    -I${GPU_DIR}/${EFFECT}
    -name=${NAME}
    -DFFX_HALF=${HALF}
    -output=${OUT_DIR}
    ${SHADER}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "ffx_sc failed for ${NAME} (${result})\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
