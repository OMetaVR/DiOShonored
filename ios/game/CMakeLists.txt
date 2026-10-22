# Dishonored iOS app shell: cross-built on Linux (clang + ld64.lld + the iPhoneOS SDK that xtool extracts), or on macOS with Xcode
# (ci/build_ios.sh, ci/ios-macos.cmake).
#
#   cmake -S ios/game -B build/ios-game -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=tools/spikes/ios_cross/ios-arm64.cmake \
#         -DSDL3_DIR=third_party/sdl3-ios-static-install/lib/cmake/SDL3
#   cmake --build build/ios-game          (or use ios/game/scripts/build_ipa.sh, which does all of it)
#
# SDL3 is always linked statically (no Frameworks/ folder: the app crashes at launch under LiveContainer when it has one).
# The d3d9 backend on iOS is a direct Metal implementation (docs/runtime_d3d9_backend.md); DXVK (+ MoltenVK) can still be linked
# statically for experiments with -DDISH_IOS_DXVK=ON (tools/spikes/ios_cross/build_dxvk_ios_static.sh builds the archives).
# -DDISH_USE_GENERATED=OFF links the empty placeholder program even when generated/ exists (boots to the import screen).
cmake_minimum_required(VERSION 3.20)
project(dishonored_ios LANGUAGES C CXX OBJC OBJCXX)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD 20)
# Linux cross builds: use the SDK's libc++ headers (the host toolchain's are newer than the libc++.dylib on the device). Xcode's
# own clang (ci/ios-macos.cmake) already pairs its headers with the SDK.
if(NOT DISH_XCODE_TOOLCHAIN AND EXISTS "${IOS_SDK}/usr/include/c++/v1")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-nostdinc++> $<$<COMPILE_LANGUAGE:CXX>:-isystem${IOS_SDK}/usr/include/c++/v1>
                      $<$<COMPILE_LANGUAGE:OBJCXX>:-nostdinc++> $<$<COMPILE_LANGUAGE:OBJCXX>:-isystem${IOS_SDK}/usr/include/c++/v1>)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DISH_IOS_DXVK "Also link DXVK d3d9 + MoltenVK statically and use them as the d3d9 backend" OFF)
option(DISH_USE_GENERATED "Link the lifter output (generated/program.c) when it exists" ON)
set(DISH_GENERATED_DIR "" CACHE PATH "lifter output directory; empty = ../../generated when it has program.c")
set(DISH_APP_EXECUTABLE "Dishonored" CACHE STRING "bundle executable name")
set(DISH_APP_BUNDLE_ID "dev.dios.dishonored" CACHE STRING "CFBundleIdentifier")
set(DXVK_STATIC_LIB_DIR "${REPO_ROOT}/third_party/dxvk/install-ios-static/lib" CACHE PATH "static DXVK archives (build_dxvk_ios_static.sh)")
set(MOLTENVK_STATIC_LIB "${REPO_ROOT}/third_party/moltenvk-release/MoltenVK/MoltenVK/static/MoltenVK.xcframework/ios-arm64/libMoltenVK.a" CACHE FILEPATH "")

set(DISH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DISH_BUILD_HOST OFF CACHE BOOL "" FORCE)
set(DISH_STATIC_DXVK ${DISH_IOS_DXVK} CACHE BOOL "" FORCE)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)   # SDL3::/Threads:: imported targets must be visible here too
add_subdirectory(${REPO_ROOT}/runtime runtime)

# ---- the program: lifter output when present, otherwise the empty placeholder (boots to the import screen)
set(_gen "${DISH_GENERATED_DIR}")
if(NOT DISH_USE_GENERATED)
  set(_gen "")
elseif(NOT _gen AND EXISTS "${REPO_ROOT}/generated/program.c")
  set(_gen "${REPO_ROOT}/generated")
endif()
if(_gen AND EXISTS "${_gen}/program.c")
  file(GLOB_RECURSE DISH_GEN_SOURCES CONFIGURE_DEPENDS "${_gen}/*.c")
  message(STATUS "dishonored_ios: recompiled program from ${_gen}")
else()
  set(DISH_GEN_SOURCES "${REPO_ROOT}/runtime/host/linux/null_program.c")
  message(STATUS "dishonored_ios: no generated/program.c, linking the placeholder program")
endif()
add_library(dish_generated OBJECT ${DISH_GEN_SOURCES})
target_include_directories(dish_generated PRIVATE ${REPO_ROOT}/include ${_gen})
target_compile_options(dish_generated PRIVATE -std=c11 -ffreestanding -O2 -mcpu=apple-a15 -fno-strict-aliasing -fwrapv -fno-math-errno
                       -ffp-contract=off -fvisibility=hidden)

add_executable(dishonored_app src/main.cpp src/screen.cpp src/libs.cpp src/download.cpp
               $<TARGET_OBJECTS:dish_runtime> $<TARGET_OBJECTS:dish_generated>)
set_target_properties(dishonored_app PROPERTIES OUTPUT_NAME ${DISH_APP_EXECUTABLE})
target_include_directories(dishonored_app PRIVATE ${REPO_ROOT}/runtime/include ${REPO_ROOT}/include)
target_compile_options(dishonored_app PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(dishonored_app PRIVATE $<TARGET_PROPERTY:dish_runtime,INTERFACE_LINK_LIBRARIES>)
target_link_libraries(dishonored_app PRIVATE ${DISH_RUNTIME_LIBS})

target_compile_definitions(dishonored_app PRIVATE DISH_IOS_STATIC=1)
# dev builds: "host:port" of tools/dev/game_files_server.py on the user's computer (the import screen offers to download the game files)
set(DISH_DEV_FILE_SERVER "" CACHE STRING "dev builds: game file server host:port (empty = off)")
if(DISH_DEV_FILE_SERVER)
  target_compile_definitions(dishonored_app PRIVATE DISH_DEV_FILE_SERVER="${DISH_DEV_FILE_SERVER}")
endif()   # SDL3 static; libs.cpp has nothing to locate unless DXVK is dynamic
if(DISH_IOS_DXVK)
  file(GLOB DXVK_STATIC_LIBS "${DXVK_STATIC_LIB_DIR}/*.a")
  if(NOT DXVK_STATIC_LIBS)
    message(FATAL_ERROR "no static DXVK archives in ${DXVK_STATIC_LIB_DIR}: run tools/spikes/ios_cross/build_dxvk_ios_static.sh")
  endif()
  if(NOT EXISTS "${MOLTENVK_STATIC_LIB}")
    message(FATAL_ERROR "MoltenVK static library not found: ${MOLTENVK_STATIC_LIB}")
  endif()
  target_link_libraries(dishonored_app PRIVATE ${DXVK_STATIC_LIBS} ${MOLTENVK_STATIC_LIB})
  target_link_libraries(dishonored_app PRIVATE "-framework IOSurface")
endif()
target_link_libraries(dishonored_app PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics"
                      "-framework UIKit" "-framework CoreFoundation" "-weak_framework MetalFX" c++)

# ---- bundle: <build>/Dishonored.app/{Dishonored, Info.plist}. The executable is linked into <build>/exe/ and the .app is assembled by
# a target that always runs (CMake's own iOS bundle logic would write a default Info.plist with an empty CFBundleIdentifier).
set_target_properties(dishonored_app PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/exe")
set(DISH_APP_DIR "${CMAKE_BINARY_DIR}/${DISH_APP_EXECUTABLE}.app")
if(NOT IOS_MIN)
  set(IOS_MIN "15.0")
endif()
set(DISH_APP_NAME "Dishonored")
configure_file(Info.plist.in "${CMAKE_BINARY_DIR}/Info.plist" @ONLY)
add_custom_target(dishonored_bundle ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${DISH_APP_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:dishonored_app>" "${DISH_APP_DIR}/${DISH_APP_EXECUTABLE}"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/Info.plist" "${DISH_APP_DIR}/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/icon/AppIcon60x60@2x.png" "${CMAKE_CURRENT_SOURCE_DIR}/icon/AppIcon60x60@3x.png"
          "${CMAKE_CURRENT_SOURCE_DIR}/icon/AppIcon76x76@2x~ipad.png" "${CMAKE_CURRENT_SOURCE_DIR}/icon/AppIcon83.5x83.5@2x~ipad.png" "${DISH_APP_DIR}/"
  DEPENDS dishonored_app
  COMMENT "assembling ${DISH_APP_EXECUTABLE}.app")

# ---- d3d9test.app: the on-device acceptance app for the d3d9 backend (runtime/tests/d3d9_bench.cpp, same program as the Linux
# d3d9_bench). The fake guest client of runtime/tests/guest_scene.h goes through the thunk layer into the Metal backend and writes
# d3d9_report.json, d3d9_shot.png, d3d9_stress.png and d3d9_log.txt to the app's Documents. It links testing.cpp for the guest_lookup of
# its fake guest functions, so it has no dish_generated objects.
option(DISH_BUILD_D3D9TEST "also build d3d9test.app" ON)
set(DISH_D3D9TEST_BUNDLE_ID "dev.dios.d3d9test.v2" CACHE STRING "CFBundleIdentifier of d3d9test.app (bump it: LiveContainer reuses an install with the same id)")
if(DISH_BUILD_D3D9TEST)
  set(_t ${REPO_ROOT}/runtime/tests)
  add_executable(d3d9test_app ${_t}/d3d9_bench.cpp ${_t}/testing.cpp $<TARGET_OBJECTS:dish_runtime>)
  set_target_properties(d3d9test_app PROPERTIES OUTPUT_NAME D3D9Test RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/exe")
  target_compile_definitions(d3d9test_app PRIVATE DISH_TESTING_NO_MAIN=1)
  target_include_directories(d3d9test_app PRIVATE ${_t} ${REPO_ROOT}/tools/spikes/d3d9_test ${REPO_ROOT}/runtime/include ${REPO_ROOT}/include)
  target_compile_options(d3d9test_app PRIVATE -Wall -Wno-unused-parameter)
  target_link_libraries(d3d9test_app PRIVATE $<TARGET_PROPERTY:dish_runtime,INTERFACE_LINK_LIBRARIES> ${DISH_RUNTIME_LIBS}
                        "-framework Metal" "-framework Foundation" "-framework QuartzCore" "-framework CoreGraphics" "-framework UIKit"
                        "-framework CoreFoundation" "-weak_framework MetalFX" c++)
  set(DISH_APP_EXECUTABLE_SAVE ${DISH_APP_EXECUTABLE})
  set(DISH_APP_BUNDLE_ID_SAVE ${DISH_APP_BUNDLE_ID})
  set(DISH_APP_EXECUTABLE D3D9Test)
  set(DISH_APP_BUNDLE_ID ${DISH_D3D9TEST_BUNDLE_ID})
  set(DISH_APP_NAME "D3D9 Test")
  configure_file(Info.plist.in "${CMAKE_BINARY_DIR}/d3d9test-Info.plist" @ONLY)
  set(DISH_APP_EXECUTABLE ${DISH_APP_EXECUTABLE_SAVE})
  set(DISH_APP_BUNDLE_ID ${DISH_APP_BUNDLE_ID_SAVE})
  set(_d ${CMAKE_BINARY_DIR}/D3D9Test.app)
  add_custom_target(d3d9test_bundle ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_d}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:d3d9test_app>" "${_d}/D3D9Test"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/d3d9test-Info.plist" "${_d}/Info.plist"
    DEPENDS d3d9test_app
    COMMENT "assembling D3D9Test.app")
endif()
