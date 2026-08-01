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

# A regression test that sits beside the code it exercises: builds <name>.cc from
# the current directory and registers it with ctest. Extra positional arguments
# are the libraries to link.
#   NO_REGISTER  build it but keep it out of ctest (corpus / manual harness)
#   AS <name>    register under a ctest name other than the executable's
#   SRCS <...>   additional translation units to compile in
# The name may carry a subdirectory ("demo/trailertest"); the target takes the
# last segment, so the test still sits in the component directory it covers.
function(recreation_add_test path)
  if(NOT RECREATION_BUILD_TOOLS)
    return()
  endif()
  cmake_parse_arguments(T "NO_REGISTER" "AS" "SRCS" ${ARGN})
  get_filename_component(name ${path} NAME)
  add_executable(${name} ${path}.cc ${T_SRCS})
  if(T_UNPARSED_ARGUMENTS)
    target_link_libraries(${name} PRIVATE ${T_UNPARSED_ARGUMENTS})
  endif()
  recreation_set_warnings(${name})
  if(NOT T_NO_REGISTER)
    if(T_AS)
      add_test(NAME ${T_AS} COMMAND ${name})
    else()
      add_test(NAME ${name} COMMAND ${name})
    endif()
  endif()
endfunction()
