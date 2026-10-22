cmake_minimum_required(VERSION 3.25)
project(Blitz2003Recomp C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generated C++ from the user's main.dol (tools/build.ps1 runs the recompiler first).
set(BLITZ_GEN_DIR "${CMAKE_SOURCE_DIR}/build/recomp_gen" CACHE PATH "Recompiler output directory")
file(GLOB BLITZ_GEN_SOURCES CONFIGURE_DEPENDS "${BLITZ_GEN_DIR}/*.cpp")

# The game module: code recompiled from the user's disc, loaded by the executable at run time.
# Without generated sources (a fresh clone, CI) only the runtime is built; the module is compiled
# from the player's disc (tools/build.ps1, tools/make_module.ps1, tools/build_linux.sh).
if (BLITZ_GEN_SOURCES)
  add_library(blitz_game SHARED ${BLITZ_GEN_SOURCES} src/runtime/module_rt.cpp)
  target_include_directories(blitz_game PRIVATE src/runtime "${BLITZ_GEN_DIR}")
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(blitz_game PRIVATE -Wno-unused-label -Wno-unused-variable -Wno-unreachable-code
      -Wno-parentheses-equality -Wno-tautological-compare)
  endif()
  if (NOT WIN32)
    # blitz_game.so like blitz_game.dll; its runtime-facing names (guest::g_mem ...) stay its own.
    set_target_properties(blitz_game PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
    target_link_options(blitz_game PRIVATE -Wl,-Bsymbolic)
  endif()
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
  src/runtime/hle_cheats.cpp
  src/runtime/hle_registry.cpp
  src/runtime/hle_dvd.cpp
  src/runtime/hle_models.cpp
  src/runtime/hle_movie.cpp
  src/runtime/hle_os.cpp
  src/runtime/hle_pad.cpp
  src/runtime/hle_thread.cpp
  src/runtime/hle_widescreen.cpp
  src/runtime/hw.cpp
  src/runtime/log.cpp
  src/runtime/memory.cpp
  src/runtime/module_loader.cpp
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

# An object library so every HLE implementation (self-registering by name) is linked in.
add_library(blitz_runtime OBJECT ${BLITZ_RUNTIME_SOURCES})
target_include_directories(blitz_runtime PUBLIC src/runtime)
target_link_libraries(blitz_runtime PUBLIC aurora::card aurora::dvd)
if (WIN32)
  target_link_libraries(blitz_runtime PUBLIC winmm)
endif()

# Headless runner: no graphics/audio, for bring-up of CPU/OS/HLE.
add_executable(blitz_headless src/app/main_headless.cpp)
if (NOT WIN32)
  set_target_properties(blitz_headless PROPERTIES EXCLUDE_FROM_ALL TRUE)  # GNU ld: circular Aurora deps
endif()
if (TARGET blitz_game)
  add_dependencies(blitz_headless blitz_game)
endif()
target_link_libraries(blitz_headless PRIVATE blitz_runtime aurora::gx aurora::vi aurora::core)

# The game: Aurora window, GX via gx_bridge.
add_library(blitz_gx OBJECT src/runtime/gx_bridge.cpp)
target_link_libraries(blitz_gx PUBLIC blitz_runtime aurora::core aurora::gx aurora::vi aurora::pad)
add_executable(blitz src/app/main_window.cpp src/app/overlay.cpp src/app/frame_shot.cpp src/app/update_check.cpp)
# The version the updater compares with the latest GitHub release (CI passes the release tag).
set(_blitz_version "dev")
if (DEFINED ENV{BLITZ_VERSION} AND NOT "$ENV{BLITZ_VERSION}" STREQUAL "")
  set(_blitz_version "$ENV{BLITZ_VERSION}")
endif()
set(BLITZ_VERSION "${_blitz_version}" CACHE STRING "Version of this build (a release tag such as v1.0)")
target_compile_definitions(blitz PRIVATE BLITZ_VERSION="${BLITZ_VERSION}")
# nlohmann/json (MIT) for the GitHub API answer: the official single header, checksum pinned.
set(BLITZ_JSON_DIR "${CMAKE_BINARY_DIR}/_deps/nlohmann_json/include")
if (NOT EXISTS "${BLITZ_JSON_DIR}/nlohmann/json.hpp")
  file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
       "${BLITZ_JSON_DIR}/nlohmann/json.hpp"
       EXPECTED_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
endif()
target_include_directories(blitz PRIVATE "${BLITZ_JSON_DIR}")
if (TARGET blitz_game)
  add_dependencies(blitz blitz_game)
endif()
target_link_libraries(blitz PRIVATE blitz_gx blitz_runtime aurora::main)
target_include_directories(blitz PRIVATE extern/aurora/lib)
set_target_properties(blitz PROPERTIES OUTPUT_NAME "NFLBlitz2003")
if (NOT WIN32)
  target_sources(blitz PRIVATE src/app/first_start.cpp)
endif()
if (WIN32)
  target_link_libraries(blitz PRIVATE comdlg32 psapi winhttp shell32)
  # Shared libraries (Dawn, DXC, SDL3, ...) next to the executable.
  add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:blitz> $<TARGET_FILE_DIR:blitz>
    COMMAND_EXPAND_LISTS)
else()
  # Find the game module and shared libraries next to the executable.
  set_target_properties(blitz blitz_headless PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()
file(GLOB BLITZ_EXTRA_DLLS "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/*.dll")
if (BLITZ_EXTRA_DLLS)
  add_custom_command(TARGET blitz POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BLITZ_EXTRA_DLLS} $<TARGET_FILE_DIR:blitz>)
endif()
