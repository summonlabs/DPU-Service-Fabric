cmake_minimum_required(VERSION 3.25)

# Independent downstream project: it consumes DPU Service Fabric only through the
# installed package, with no access to the source tree, no subdirectory inclusion
# and no shared build flags.
project(DPUServiceFabricConsumer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(DPUServiceFabric 1.0 REQUIRED)

if(NOT TARGET dpu::fabric)
  message(FATAL_ERROR "the package did not export the dpu::fabric target")
endif()

# The repository's warning policy must not leak into consumers. The consumer sets
# its own strict flags; if the package also contributed any, this build would be
# silently stricter than the consumer asked for.
get_target_property(dpuf_leaked_options dpu::fabric INTERFACE_COMPILE_OPTIONS)
if(dpuf_leaked_options)
  message(FATAL_ERROR "warning or build flags leaked downstream: ${dpuf_leaked_options}")
endif()
get_target_property(dpuf_leaked_definitions dpu::fabric INTERFACE_COMPILE_DEFINITIONS)
if(dpuf_leaked_definitions)
  message(FATAL_ERROR "definitions leaked downstream: ${dpuf_leaked_definitions}")
endif()
get_target_property(dpuf_interface_includes dpu::fabric INTERFACE_INCLUDE_DIRECTORIES)
if(NOT dpuf_interface_includes)
  message(FATAL_ERROR "the exported target does not carry its include directory")
endif()
foreach(include_dir IN LISTS dpuf_interface_includes)
  if(include_dir MATCHES "/src" OR include_dir MATCHES "\\src")
    message(FATAL_ERROR "a private source directory leaked downstream: ${include_dir}")
  endif()
endforeach()

add_executable(dpuf_consumer main.cpp)
target_link_libraries(dpuf_consumer PRIVATE dpu::fabric)

if(MSVC)
  target_compile_options(dpuf_consumer PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
else()
  target_compile_options(dpuf_consumer PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()

install(TARGETS dpuf_consumer RUNTIME DESTINATION bin)
