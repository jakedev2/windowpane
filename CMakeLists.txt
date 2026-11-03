cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Use the 64-bit MSVC compiler. The default Visual Studio toolset runs the 32-bit cl.exe, which runs out of memory
# during "Generating Code..." on large Geode files and dies with exit code -1073741819 (0xC0000005).
if (CMAKE_GENERATOR MATCHES "Visual Studio")
    set(CMAKE_GENERATOR_TOOLSET "host=x64" CACHE STRING "" FORCE)
endif()

project(Windowpane VERSION 2.5.2)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)

if (MSVC)
    # cl.exe previously crashed (0xC0000005) in whole-program code generation. Keep /GL- and one compiler process
    # at a time. The UI remains unoptimised below; the analysis hot files selectively get /O2 after setup_geode_mod.
    target_compile_options(${PROJECT_NAME} PRIVATE /MP1 /bigobj /Od /GL-)
    # Give every source file its own compiler invocation (a unique define makes the command lines differ), so that if
    # the compiler ever crashes again the last file name printed is the culprit.
    set(WP_UNIT_COUNTER 0)
    foreach(WP_SRC ${SOURCES})
        math(EXPR WP_UNIT_COUNTER "${WP_UNIT_COUNTER}+1")
        set_source_files_properties(${WP_SRC} PROPERTIES COMPILE_DEFINITIONS "WP_UNIT=${WP_UNIT_COUNTER}")
    endforeach()
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Define the GEODE_SDK environment variable.")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})

# --- MSVC code-generation crash workaround -------------------------------------------------------------------
# The old log showed cl.exe dying in whole-program code generation. Only the analysis hot files are optimised by
# default, without /GL. If a compiler still crashes, -DWP_NOOPT_ALL=ON restores the old all-/Od fallback.
option(WP_NOOPT_ALL "Build all Windowpane sources with /Od" OFF)
if (MSVC)
    if (WP_NOOPT_ALL)
        target_compile_options(${PROJECT_NAME} PRIVATE /Od)
    else()
        # The analyzer drives the game's already-compiled physics, but its own state capture, comparisons, hooks and
        # snapshot restore run for every simulated tick. Do not force those hot files through /Od with the UI sources.
        # /GL- stays off: these files get the normal optimiser without the whole-program code-generation path that crashed.
        set_source_files_properties(
            src/sim/Analyzer.cpp src/sim/Stepper.cpp src/sim/Snapshot.cpp src/sim/PlayerExtra.cpp src/hooks/Hooks.cpp
            PROPERTIES COMPILE_OPTIONS "/O2")
    endif()
endif()
