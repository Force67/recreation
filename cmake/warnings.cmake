function(recreation_set_warnings target)
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
