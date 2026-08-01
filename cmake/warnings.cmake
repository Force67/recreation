# recreation's game components are included repo-root relative
# ("components/bethesda/nif.h"). rx's engine/ header root holds directories of
# the same name (weather/, script/, audio/), so an unqualified "weather/weather.h"
# would resolve by -I order; the components/ prefix makes it unambiguous.
function(recreation_set_warnings target)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR})
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
  target_include_directories(recreation_${name} PUBLIC ${CMAKE_SOURCE_DIR})
  recreation_set_warnings(recreation_${name})
endfunction()
