# rx and recreation both use `engine/` as their header root, so a module name
# present in both (weather/) resolves by -I order, and rx's root lands first for
# any target that links an rx module. recreation's own headers must win in
# recreation's build: prepend its root so "weather/weather.h" is the game
# weather module, not rx's cloudscape weather-state layer.
function(recreation_prefer_own_headers target)
  target_include_directories(${target} BEFORE PRIVATE ${CMAKE_SOURCE_DIR}/engine)
endfunction()

function(recreation_set_warnings target)
  recreation_prefer_own_headers(${target})
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    # missing-field-initializers fights designated init of vulkan structs
    # where value initializing the rest is exactly the point.
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Wno-unused-parameter
      -Wno-missing-field-initializers)
  endif()
endfunction()

function(recreation_add_module name)
  add_library(recreation_${name} STATIC ${ARGN})
  add_library(recreation::${name} ALIAS recreation_${name})
  target_include_directories(recreation_${name} PUBLIC ${CMAKE_SOURCE_DIR}/engine)
  recreation_set_warnings(recreation_${name})
endfunction()
