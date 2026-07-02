# NVIDIA DLSS (NGX) SDK, downloaded via tools/get_dlss.sh. Ships headers and a
# prebuilt static lib (libnvsdk_ngx.a) for x86_64 only. On other architectures
# (GB10/DGX aarch64) the driver's own libnvidia-ngx.so.1 exports the same SDK
# ABI, so ngx_shim/ngx_shim.cc dlopens it and forwards - see the shim for the
# one signature that differs. The DLSS inference snippets are separate .so
# files loaded at runtime; NGX is told where to find the app-shipped ones
# through RECREATION_DLSS_LIB_DIR (x86_64 SDK snippets; on aarch64 the driver
# must supply them, e.g. via NGX OTA, or the features report unavailable and
# the engine falls back).
set(DLSS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/DLSS)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  add_library(recreation_dlss STATIC IMPORTED GLOBAL)
  set_target_properties(recreation_dlss PROPERTIES
    IMPORTED_LOCATION ${DLSS_DIR}/lib/Linux_x86_64/libnvsdk_ngx.a
    INTERFACE_INCLUDE_DIRECTORIES ${DLSS_DIR}/include
    INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}"
    INTERFACE_COMPILE_DEFINITIONS "RECREATION_DLSS_LIB_DIR=\"${DLSS_DIR}/lib/Linux_x86_64/rel\"")
else()
  add_library(recreation_dlss STATIC ${CMAKE_CURRENT_SOURCE_DIR}/ngx_shim/ngx_shim.cc)
  target_include_directories(recreation_dlss PUBLIC ${DLSS_DIR}/include)
  target_link_libraries(recreation_dlss PUBLIC ${CMAKE_DL_LIBS} volk)
  # No app-shipped snippets on this arch; NGX searches its own driver paths.
  target_compile_definitions(recreation_dlss INTERFACE "RECREATION_DLSS_LIB_DIR=\"\"")
endif()
add_library(recreation::dlss ALIAS recreation_dlss)
