# kinema: self-contained skeletal animation runtime. No dependencies beyond
# the C++ standard library; drop the directory into any project and
# add_subdirectory it. The optional Jolt adapter is header-only
# (include/kinema/jolt_adapter.h) and only needs Jolt headers at the
# including translation unit.

cmake_minimum_required(VERSION 3.24)
project(kinema LANGUAGES CXX)

add_library(kinema STATIC
  src/clip.cc
  src/blend.cc
)
add_library(kinema::kinema ALIAS kinema)
target_include_directories(kinema PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(kinema PUBLIC cxx_std_17)
set_target_properties(kinema PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(KINEMA_BUILD_TESTS "Build the kinema unit tests" OFF)
if(KINEMA_BUILD_TESTS)
  add_executable(kinematest test/kinema_test.cc)
  target_link_libraries(kinematest PRIVATE kinema)
  add_test(NAME kinematest COMMAND kinematest)
endif()
