cmake_minimum_required(VERSION 3.25)

project(
  DPUServiceFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral C++20 runtime that deploys and governs infrastructure services on DPUs"
  HOMEPAGE_URL "https://github.com/summonlabs/DPU-Service-Fabric"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(DPUF_BUILD_TESTS "Build the DPU Service Fabric test suite" ON)
option(DPUF_BUILD_TOOLS "Build the dpufabric inspection and transport tool" ON)
option(DPUF_BUILD_BENCHMARKS "Build the completion-measuring benchmarks" ON)
option(DPUF_BUILD_SHARED "Build the runtime as a shared library" OFF)
option(DPUF_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(DPUF_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)
option(DPUF_ENABLE_STRICT_CONVERSIONS "Enable extra narrowing/sign-conversion warnings on GCC/Clang"
       OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE
      Release
      CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Warning policy: first-party only, never propagated downstream
# ---------------------------------------------------------------------------

function(dpuf_apply_warnings target)
  if(MSVC)
    target_compile_options(
      ${target}
      PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor
              /wd4996 # deprecated CRT names are not a correctness concern here
    )
    if(DPUF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wnon-virtual-dtor
              -Wold-style-cast
              -Wcast-align
              -Wunused
              -Woverloaded-virtual
              -Wconversion
              -Wsign-conversion
              -Wdouble-promotion
              -Wformat=2
              -Wimplicit-fallthrough)
    if(DPUF_ENABLE_STRICT_CONVERSIONS)
      target_compile_options(${target} PRIVATE -Werror=conversion -Werror=sign-conversion)
    endif()
    if(DPUF_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

if(DPUF_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Runtime library
# ---------------------------------------------------------------------------

set(DPUF_LIBRARY_TYPE STATIC)
if(DPUF_BUILD_SHARED)
  set(DPUF_LIBRARY_TYPE SHARED)
endif()

set(DPUF_CORE_SOURCES
    src/version.cpp
    src/core/reason.cpp
    src/core/result.cpp
    src/core/digest.cpp
    src/core/json.cpp
    src/core/archive.cpp
    src/core/resources.cpp
    src/core/bounded.cpp
    src/model/model.cpp
    src/engine/eligibility.cpp
    src/engine/dependency.cpp
    src/engine/authority.cpp
    src/engine/planner.cpp
    src/persist/store.cpp
    src/engine/runtime.cpp
    src/transport/frame.cpp
    src/transport/socket.cpp
    src/transport/server.cpp
    src/transport/client.cpp)

add_library(dpufabric ${DPUF_LIBRARY_TYPE} ${DPUF_CORE_SOURCES})
add_library(dpu::fabric ALIAS dpufabric)

target_include_directories(
  dpufabric
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(dpufabric PUBLIC cxx_std_20)

set_target_properties(
  dpufabric
  PROPERTIES VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR}
             CXX_VISIBILITY_PRESET hidden
             VISIBILITY_INLINES_HIDDEN ON
             EXPORT_NAME fabric)

if(MSVC)
  target_compile_definitions(dpufabric PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(Threads REQUIRED)

if(MSVC)
  target_link_libraries(dpufabric PRIVATE ws2_32)
else()
  target_link_libraries(dpufabric PUBLIC Threads::Threads)
endif()

dpuf_apply_warnings(dpufabric)

# ---------------------------------------------------------------------------
# Installation and CMake package
# ---------------------------------------------------------------------------

install(
  TARGETS dpufabric
  EXPORT DPUServiceFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(DPUF_CMAKE_INSTALL_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/DPUServiceFabric)

install(
  EXPORT DPUServiceFabricTargets
  FILE DPUServiceFabricTargets.cmake
  NAMESPACE dpu::
  DESTINATION ${DPUF_CMAKE_INSTALL_DIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/DPUServiceFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/DPUServiceFabricConfig.cmake
  INSTALL_DESTINATION ${DPUF_CMAKE_INSTALL_DIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/DPUServiceFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/DPUServiceFabricConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/DPUServiceFabricConfigVersion.cmake
        DESTINATION ${DPUF_CMAKE_INSTALL_DIR})

# ---------------------------------------------------------------------------
# Tools, tests, benchmarks
# ---------------------------------------------------------------------------

if(DPUF_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(DPUF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DPUF_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
