cmake_minimum_required(VERSION 3.20)

project(agent_runtime
  VERSION 1.0.0
  DESCRIPTION "Execution lifecycle authority for long-running autonomous agents"
  HOMEPAGE_URL "https://github.com/pngen/Agent-Runtime"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(AGENT_RUNTIME_IS_TOP_LEVEL ON)
else()
  set(AGENT_RUNTIME_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(AGENT_RUNTIME_BUILD_TESTS "Build the Agent Runtime test suite" ${AGENT_RUNTIME_IS_TOP_LEVEL})
option(AGENT_RUNTIME_BUILD_EXAMPLES "Build the Agent Runtime examples" ${AGENT_RUNTIME_IS_TOP_LEVEL})
option(AGENT_RUNTIME_BUILD_TOOLS "Build the Agent Runtime inspection tool" ${AGENT_RUNTIME_IS_TOP_LEVEL})
option(AGENT_RUNTIME_BUILD_BENCHMARKS "Build the Agent Runtime benchmarks" ${AGENT_RUNTIME_IS_TOP_LEVEL})
option(AGENT_RUNTIME_BUILD_REFERENCE "Build the reference multiprocess architecture" ${AGENT_RUNTIME_IS_TOP_LEVEL})
option(AGENT_RUNTIME_BUILD_CUDA_PROOF "Build the optional CUDA reference action proof" ON)
option(AGENT_RUNTIME_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(AGENT_RUNTIME_ENABLE_SHARED "Build Agent Runtime as a shared library" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(AGENT_RUNTIME_ENABLE_SHARED)
  set(AGENT_RUNTIME_LIBRARY_TYPE SHARED)
else()
  set(AGENT_RUNTIME_LIBRARY_TYPE STATIC)
endif()

# ---------------------------------------------------------------------------
# Warnings: strict, and never suppressed to fake cleanliness.
# ---------------------------------------------------------------------------
add_library(agent_runtime_warnings INTERFACE)
# Options are restricted to C++ compiles: nvcc must not receive host-only flags.
if(MSVC)
  target_compile_options(agent_runtime_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/permissive-;/utf-8;/EHsc;/Zc:__cplusplus;/Zc:preprocessor;/wd4324>)
else()
  target_compile_options(agent_runtime_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror;-Wshadow;-Wconversion;-Wsign-conversion;-Wold-style-cast;-Wnon-virtual-dtor;-Woverloaded-virtual;-Wdouble-promotion>)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(AGENT_RUNTIME_CORE_SOURCES
  src/authority.cpp
  src/clock.cpp
  src/detail/crc32c.cpp
  src/detail/sha256.cpp
  src/ids.cpp
  src/limits.cpp
  src/lifecycle.cpp
  src/outcome.cpp
  src/persistence.cpp
  src/policy.cpp
  src/reference_backends.cpp
  src/runtime.cpp
  src/runtime_dispatch.cpp
  src/runtime_inspect.cpp
  src/runtime_lifecycle.cpp
  src/runtime_persist.cpp)

set(AGENT_RUNTIME_PUBLIC_HEADERS
  include/agent_runtime/agent_runtime.hpp
  include/agent_runtime/authority.hpp
  include/agent_runtime/backends.hpp
  include/agent_runtime/clock.hpp
  include/agent_runtime/ids.hpp
  include/agent_runtime/limits.hpp
  include/agent_runtime/lifecycle.hpp
  include/agent_runtime/outcome.hpp
  include/agent_runtime/persistence.hpp
  include/agent_runtime/policy.hpp
  include/agent_runtime/runtime.hpp
  include/agent_runtime/version.hpp)

add_library(agent_runtime ${AGENT_RUNTIME_LIBRARY_TYPE} ${AGENT_RUNTIME_CORE_SOURCES})
add_library(agent_runtime::agent_runtime ALIAS agent_runtime)
target_include_directories(agent_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(agent_runtime PUBLIC cxx_std_20)
target_link_libraries(agent_runtime PRIVATE $<BUILD_INTERFACE:agent_runtime_warnings>)
set_target_properties(agent_runtime PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME agent_runtime
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)

if(WIN32)
  target_compile_definitions(agent_runtime PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS)
endif()

if(AGENT_RUNTIME_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(agent_runtime PUBLIC /fsanitize=address)
    target_link_options(agent_runtime PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(agent_runtime PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(agent_runtime PUBLIC -fsanitize=address)
  endif()
endif()

# ---------------------------------------------------------------------------
# Optional CUDA reference action proof
# ---------------------------------------------------------------------------
set(AGENT_RUNTIME_CUDA_FOUND OFF)
if(AGENT_RUNTIME_BUILD_CUDA_PROOF)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
      set(CMAKE_CUDA_ARCHITECTURES native)
    endif()
    set(AGENT_RUNTIME_CUDA_FOUND ON)
    message(STATUS "Agent Runtime: CUDA reference proof enabled with ${CMAKE_CUDA_COMPILER}")
  else()
    message(STATUS "Agent Runtime: CUDA not available; the optional CUDA proof is not built")
  endif()
endif()

if(AGENT_RUNTIME_CUDA_FOUND)
  find_package(CUDAToolkit REQUIRED)
  add_library(agent_runtime_cuda_reference STATIC
    cuda/cuda_reference_action.cpp
    cuda/cuda_reference_kernel.cu)
  add_library(agent_runtime::cuda_reference ALIAS agent_runtime_cuda_reference)
  target_include_directories(agent_runtime_cuda_reference
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(agent_runtime_cuda_reference
    PUBLIC agent_runtime CUDA::cudart
    PRIVATE $<BUILD_INTERFACE:agent_runtime_warnings>)
  set_target_properties(agent_runtime_cuda_reference PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION OFF)
  target_compile_definitions(agent_runtime_cuda_reference PRIVATE AGENT_RUNTIME_HAS_CUDA=1)
  set_source_files_properties(cuda/cuda_reference_kernel.cu PROPERTIES LANGUAGE CUDA)
  if(MSVC)
    target_compile_options(agent_runtime_cuda_reference PRIVATE
      $<$<COMPILE_LANGUAGE:CUDA>:--Werror=all-warnings;-Xcompiler=/W4,-WX>)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests, examples, tools, benchmarks, reference architecture
# ---------------------------------------------------------------------------
if(AGENT_RUNTIME_BUILD_REFERENCE)
  add_subdirectory(reference)
endif()

if(AGENT_RUNTIME_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(AGENT_RUNTIME_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(AGENT_RUNTIME_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(AGENT_RUNTIME_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Install rules and package config
# ---------------------------------------------------------------------------
install(TARGETS agent_runtime
  EXPORT agent_runtimeTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(AGENT_RUNTIME_CUDA_FOUND)
  install(TARGETS agent_runtime_cuda_reference
    EXPORT agent_runtimeTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Implementation detail headers are not part of the installed interface.
install(DIRECTORY include/agent_runtime DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  PATTERN "detail" EXCLUDE)

install(EXPORT agent_runtimeTargets
  FILE agent_runtimeTargets.cmake
  NAMESPACE agent_runtime::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_runtime)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/agent_runtimeConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/agent_runtimeConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_runtime)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/agent_runtimeConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/agent_runtimeConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/agent_runtimeConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agent_runtime)

install(FILES LICENSE README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/agent_runtime
  OPTIONAL)

message(STATUS "Agent Runtime ${PROJECT_VERSION}: CUDA proof=${AGENT_RUNTIME_CUDA_FOUND} ASan=${AGENT_RUNTIME_ENABLE_ASAN}")
