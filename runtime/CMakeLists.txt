cmake_minimum_required(VERSION 3.20)
# guest_abi.h declares `_Thread_local` inside extern "C", which g++ rejects in C++ mode: prefer clang (the iOS toolchain is clang too).
if(NOT CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  find_program(_clangxx clang++)
  find_program(_clang clang)
  if(_clangxx AND _clang)
    set(CMAKE_CXX_COMPILER ${_clangxx})
    set(CMAKE_C_COMPILER ${_clang})
  endif()
endif()
project(dishonored_runtime LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(DISH_BUILD_TESTS "Build the ctest suite" ON)
option(DISH_BUILD_HOST "Build host/linux (needs SDL3)" ON)
set(DISH_GENERATED_DIR "" CACHE PATH "Directory with lifter output (program.c + shards); empty = auto-detect ../generated")

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# ---- the runtime library -------------------------------------------------------------------------------------
# An OBJECT library on purpose: HLE functions register themselves from static initialisers, which a static archive
# would silently drop.
file(GLOB DISH_SOURCES CONFIGURE_DEPENDS
  core/*.cpp loader/*.cpp hle/*.cpp overrides/*.cpp platform/*.cpp)
add_library(dish_runtime OBJECT ${DISH_SOURCES})
# portable parts of the Metal d3d9 backend (texel codecs, shader translation): compiled everywhere so they are unit-tested on Linux
set(DISH_MOJOSHADER_DIR "${REPO_ROOT}/external/mojoshader")   # vendored (zlib), with the local patches in LOCAL_CHANGES.md
target_sources(dish_runtime PRIVATE platform/d3d9_metal/texcodec.cpp platform/d3d9_metal/shader_xlate.cpp
  ${DISH_MOJOSHADER_DIR}/mojoshader.c ${DISH_MOJOSHADER_DIR}/mojoshader_common.c
  ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_d3d.c
  ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_metal.c)
# MojoShader is vendored unmodified (zlib). Only the D3D parser and the Metal profile are built; every other profile is compiled out.
set_source_files_properties(${DISH_MOJOSHADER_DIR}/mojoshader.c ${DISH_MOJOSHADER_DIR}/mojoshader_common.c
  ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_common.c ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_d3d.c
  ${DISH_MOJOSHADER_DIR}/profiles/mojoshader_profile_metal.c PROPERTIES
  COMPILE_OPTIONS "-w;-fno-strict-aliasing;-std=gnu11"
  COMPILE_DEFINITIONS "SUPPORT_PROFILE_HLSL=0;SUPPORT_PROFILE_GLSL=0;SUPPORT_PROFILE_GLSL120=0;SUPPORT_PROFILE_GLSLES=0;SUPPORT_PROFILE_GLSLES3=0;SUPPORT_PROFILE_ARB1=0;SUPPORT_PROFILE_ARB1_NV=0;SUPPORT_PROFILE_SPIRV=0;SUPPORT_PROFILE_GLSPIRV=0;SUPPORT_PROFILE_BYTECODE=0")
target_include_directories(dish_runtime PRIVATE ${DISH_MOJOSHADER_DIR})
target_include_directories(dish_runtime PUBLIC include ${REPO_ROOT}/include)
target_compile_options(dish_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-exceptions)
target_compile_definitions(dish_runtime PRIVATE _FILE_OFFSET_BITS=64)
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(dish_runtime PUBLIC _Thread_local=thread_local)
endif()
find_package(Threads REQUIRED)
set(DISH_RUNTIME_LIBS Threads::Threads ${CMAKE_DL_LIBS})

# ---- platform layer: SDL3 (window, events, gamepad, audio) and the DXVK-native d3d9 headers (only used by platform/d3d9*.cpp)
option(DISH_STATIC_DXVK "d3d9 layer calls the DXVK Direct3DCreate9 linked into the executable instead of dlopen'ing it" OFF)
if(APPLE)
  # iOS: SDL3 comes from a cross-built install (static or dynamic): pass -DSDL3_DIR=<prefix>/lib/cmake/SDL3
  find_package(SDL3 CONFIG REQUIRED)
  if(TARGET SDL3::SDL3-static)
    set(_sdl_target SDL3::SDL3-static)
  else()
    set(_sdl_target SDL3::SDL3-shared)
  endif()
  list(APPEND DISH_RUNTIME_LIBS ${_sdl_target})
  target_link_libraries(dish_runtime PUBLIC ${_sdl_target})   # object library: only usage requirements (include dirs) propagate
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL3 REQUIRED sdl3)
  target_include_directories(dish_runtime PUBLIC ${SDL3_INCLUDE_DIRS})
  include_directories(${SDL3_INCLUDE_DIRS})   # tests, benches and the host include SDL too (SDL3 may live outside /usr/include)
  list(APPEND DISH_RUNTIME_LIBS ${SDL3_LIBRARIES})
  link_directories(${SDL3_LIBRARY_DIRS})
endif()
# A local DXVK-native build (headers + library) when present, otherwise the vendored headers only (enough for the Metal backend
# and for building; Linux runs then need DISH_DXVK_LIB pointing at a libdxvk_d3d9.so).
if(EXISTS "${REPO_ROOT}/third_party/dxvk/install-linux/include/dxvk")
  set(_dxvk_default "${REPO_ROOT}/third_party/dxvk/install-linux")
else()
  set(_dxvk_default "${REPO_ROOT}/external/dxvk-headers")
endif()
set(DISH_DXVK_DIR "${_dxvk_default}" CACHE PATH "DXVK-native install (headers for d3d9; the library is dlopen'ed at run time unless DISH_STATIC_DXVK)")
if(APPLE)
  # direct D3D9 -> Metal backend (Objective-C++, ARC); needs the host D3D9 headers only
  file(GLOB DISH_METAL_SOURCES CONFIGURE_DEPENDS platform/d3d9_metal/*.mm)
  target_sources(dish_runtime PRIVATE ${DISH_METAL_SOURCES})
  set_source_files_properties(${DISH_METAL_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-isystem;${DISH_DXVK_DIR}/include/dxvk")
endif()
file(GLOB DISH_D3D9_SOURCES CONFIGURE_DEPENDS platform/d3d9*.cpp)
set(_d3d9_defs "DISH_D3D9_TU=1;DISH_DXVK_DEFAULT_LIB=\"${DISH_DXVK_DIR}/lib/libdxvk_d3d9.so.0\"")
if(DISH_STATIC_DXVK)
  list(APPEND _d3d9_defs "DISH_STATIC_DXVK=1")
endif()
set_source_files_properties(${DISH_D3D9_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-isystem;${DISH_DXVK_DIR}/include/dxvk;-fexceptions"
  COMPILE_DEFINITIONS "${_d3d9_defs}")

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(DISH_RUNTIME_LIBS ${DISH_RUNTIME_LIBS} PARENT_SCOPE)   # used via add_subdirectory (ios/game)
endif()

if(DISH_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DISH_BUILD_HOST)
  add_subdirectory(host/linux)
endif()
