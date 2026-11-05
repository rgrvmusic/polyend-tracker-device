# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gerard Braad (Regroove | RGRV)

cmake_minimum_required(VERSION 3.22.1)

project("tracker")

include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
    set(CMAKE_BUILD_PARALLEL_LEVEL ${N})
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache for faster rebuilds")
endif()

# This file sits at <repo>/android/app/src/main/cpp: five levels up is the
# repo root, where the emulator lives. SDL3 and Dear ImGui are not vendored;
# they are expected in third_party/ or wherever these cache paths point.
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..)
set(SDL3_DIR ${ROOT}/third_party/SDL3 CACHE PATH "SDL3 source tree")
set(IMGUI_DIR ${ROOT}/third_party/imgui CACHE PATH "Dear ImGui source tree")
set(PANEL_DIR ${ROOT}/emulator/panel/fire)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL3_DIR} SDL3 EXCLUDE_FROM_ALL)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    # json.c is the panel's JSON reader; the door's frames are nested JSON and a
    # field scan would find the wrong object.
    ${PANEL_DIR}
    ${SDL3_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

add_library(main SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/child.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/screen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/panel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/midi_bridge.cpp
    ${PANEL_DIR}/json.c
    # Pins the process ABI to armeabi-v7a: abiFilters alone is a filter over an
    # empty set of libraries.  Read the file before removing it.
    ${CMAKE_CURRENT_SOURCE_DIR}/abi_anchor.c
    ${IMGUI_SOURCES}
)

find_library(log-lib log)
find_library(GLESv3-lib GLESv3)
find_library(android-lib android)

target_compile_definitions(main PRIVATE __ANDROID__)

# SDL finds SDL_main through the dynamic table, and the JNI methods the Java
# side declares (MidiHandler, MainActivity.nativeStop) are looked up the same
# way.
set_target_properties(main PROPERTIES LINK_FLAGS "-Wl,--export-dynamic")

target_link_libraries(main
    SDL3::SDL3
    ${log-lib}
    ${GLESv3-lib}
    ${android-lib}
    m
    dl
)
