cmake_minimum_required(VERSION 3.25)
project(Blitz2003Recomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's main.dol (tools/build.ps1 runs the recompiler first).
set(BLITZ_GEN_DIR "${CMAKE_SOURCE_DIR}/build/recomp_gen" CACHE PATH "Recompiler output directory")
file(GLOB BLITZ_GEN_SOURCES CONFIGURE_DEPENDS "${BLITZ_GEN_DIR}/*.cpp")

add_library(blitz_game STATIC ${BLITZ_GEN_SOURCES})
target_include_directories(blitz_game PUBLIC src/runtime "${BLITZ_GEN_DIR}")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(blitz_game PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-unreachable-code
    -Wno-parentheses-equality -Wno-tautological-compare)
endif()

set(BLITZ_RUNTIME_SOURCES
  src/runtime/audio_out.cpp
  src/runtime/boot.cpp
  src/runtime/boot_args.cpp
  src/runtime/cpu.cpp
  src/runtime/disc.cpp
  src/runtime/dsp_ax.cpp
  src/runtime/gx_fifo.cpp
  src/runtime/hle_card.cpp
  src/runtime/hle_dvd.cpp
  src/runtime/hle_os.cpp
  src/runtime/hle_pad.cpp
  src/runtime/hle_thread.cpp
  src/runtime/hw.cpp
  src/runtime/log.cpp
  src/runtime/memory.cpp
  src/runtime/pad_input.cpp
  src/runtime/sched.cpp
  src/runtime/settings.cpp
)

# Aurora: GX on WebGPU (Dawn) + SDL3 window/input.
set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
option(BLITZ_AURORA_CHECKS "Build Aurora's GX with debug logging/validation" OFF)
if (BLITZ_AURORA_CHECKS)
  target_compile_options(aurora_gx PRIVATE -UNDEBUG)
  target_compile_options(aurora_core PRIVATE -UNDEBUG)
endif()

add_library(blitz_runtime STATIC ${BLITZ_RUNTIME_SOURCES})
target_include_directories(blitz_runtime PUBLIC src/runtime)
target_link_libraries(blitz_runtime PUBLIC blitz_game aurora::card aurora::dvd)
if (WIN32)
  target_link_libraries(blitz_runtime PUBLIC winmm)
endif()

# Headless runner: no graphics/audio, for bring-up of CPU/OS/HLE.
add_executable(blitz_headless src/app/main_headless.cpp)
target_link_libraries(blitz_headless PRIVATE blitz_runtime aurora::gx aurora::vi)

# The game: Aurora window, GX via gx_bridge.
add_library(blitz_gx STATIC src/runtime/gx_bridge.cpp)
target_link_libraries(blitz_gx PUBLIC blitz_runtime aurora::core aurora::gx aurora::vi aurora::pad)
add_executable(blitz src/app/main_window.cpp src/app/overlay.cpp)
target_link_libraries(blitz PRIVATE blitz_gx aurora::main)
target_include_directories(blitz PRIVATE extern/aurora/lib)
if (WIN32)
  target_link_libraries(blitz PRIVATE comdlg32 psapi)
  set_target_properties(blitz PROPERTIES OUTPUT_NAME "NFLBlitz2003")
endif()
# Shared libraries (Dawn, DXC, SDL3, ...) next to the executable.
add_custom_command(TARGET blitz POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:blitz> $<TARGET_FILE_DIR:blitz>
  COMMAND_EXPAND_LISTS)
file(GLOB BLITZ_EXTRA_DLLS "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/*.dll")
if (BLITZ_EXTRA_DLLS)
  add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BLITZ_EXTRA_DLLS} $<TARGET_FILE_DIR:blitz>)
endif()
