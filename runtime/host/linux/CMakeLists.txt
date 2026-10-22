# Linux host executable: dishonored_linux

# Lifter output (generated/program.c + shards): compiled as C with the flags of docs/guest_abi.md section 9.
option(DISH_USE_GENERATED "Link the lifter output (generated/program.c) when it exists" ON)
set(_gen "${DISH_GENERATED_DIR}")
if(NOT _gen AND DISH_USE_GENERATED AND EXISTS "${REPO_ROOT}/generated/program.c")
  set(_gen "${REPO_ROOT}/generated")
endif()
set(DISH_GEN_SOURCES "")
if(_gen AND DISH_USE_GENERATED AND EXISTS "${_gen}/program.c")
  file(GLOB_RECURSE DISH_GEN_SOURCES CONFIGURE_DEPENDS "${_gen}/*.c")
  message(STATUS "dishonored_linux: using generated program from ${_gen} (${CMAKE_CURRENT_LIST_DIR})")
else()
  set(DISH_GEN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/null_program.c")
  message(STATUS "dishonored_linux: no generated/program.c found, linking the empty placeholder program")
endif()

add_library(dish_generated OBJECT ${DISH_GEN_SOURCES})
target_include_directories(dish_generated PRIVATE ${REPO_ROOT}/include ${_gen})
set(GEN_FLAGS -std=c11 -ffreestanding -O2 -fno-strict-aliasing -fwrapv -fno-math-errno -ffp-contract=off -fvisibility=hidden)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  list(APPEND GEN_FLAGS -march=x86-64-v2)
endif()
target_compile_options(dish_generated PRIVATE ${GEN_FLAGS})

add_executable(dishonored_linux main.cpp $<TARGET_OBJECTS:dish_runtime> $<TARGET_OBJECTS:dish_generated>)
target_include_directories(dishonored_linux PRIVATE ${REPO_ROOT}/runtime/include ${REPO_ROOT}/include)
target_link_libraries(dishonored_linux PRIVATE ${DISH_RUNTIME_LIBS})
target_link_options(dishonored_linux PRIVATE -rdynamic)   # guest functions (g_XXXXXXXX) show up in fault backtraces
target_include_directories(dishonored_linux PRIVATE ${SDL3_INCLUDE_DIRS})
