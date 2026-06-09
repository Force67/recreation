cmake_minimum_required(VERSION 3.24)
project(recreation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RECREATION_ENABLE_VALIDATION "Enable vulkan validation layers in debug" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(warnings)

find_package(Vulkan QUIET)
find_package(ZLIB QUIET)

if(Vulkan_FOUND)
  message(STATUS "vulkan found, full renderer enabled")
else()
  message(STATUS "vulkan not found, renderer builds as stub")
endif()

set(RECREATION_MODULES
  core
  ecs
  asset
  render
  bethesda
  world
  net
  script
)

foreach(module ${RECREATION_MODULES})
  add_subdirectory(engine/${module})
endforeach()
