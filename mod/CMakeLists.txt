cmake_minimum_required(VERSION 3.20)
project(GlintSpotter LANGUAGES CXX)

# Glint Spotter: an ASI plugin for Crimson Desert. Builds GlintSpotter.asi, which
# an ASI loader (winmm.dll from Ultimate ASI Loader) injects, the same loader
# Master Looter already uses.
#
# 0.1.0 is a probe. No ImGui, no DirectX, no detours, so no FetchContent and no
# third-party code at all. It scans for two UI objects and writes a log.

if(NOT WIN32)
    message(FATAL_ERROR "Glint Spotter targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT, no redist needed

add_library(GlintSpotter SHARED
    src/dllmain.cpp
    src/core/log.cpp
    src/core/mod.cpp
    src/game/rtti.cpp
    src/game/scan.cpp
    src/game/typescan.cpp
)

target_include_directories(GlintSpotter PRIVATE src)
# QueryWorkingSetEx, for skipping pages that are not resident.
target_link_libraries(GlintSpotter PRIVATE psapi)
target_compile_definitions(GlintSpotter PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(GlintSpotter PRIVATE /utf-8 /W3)

# Bisect switch for load-time faults. 3 is the real plugin; lower numbers drop
# one layer each so a crash inside LoadLibrary can be cornered without launching
# the game. 0 is a DLL whose DllMain does nothing at all.
#   cmake -DGS_STAGE=0 ...
if(DEFINED ENV{GS_STAGE})
    set(GS_STAGE $ENV{GS_STAGE})
else()
    set(GS_STAGE 3)
endif()
message(STATUS "GS_STAGE=${GS_STAGE}")
target_compile_definitions(GlintSpotter PRIVATE GS_STAGE=${GS_STAGE})

set_target_properties(GlintSpotter PROPERTIES PREFIX "" SUFFIX ".asi" OUTPUT_NAME "GlintSpotter")

if(MSVC)
    target_compile_options(GlintSpotter PRIVATE /guard:cf)
    target_link_options(GlintSpotter PRIVATE /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf)
endif()

add_custom_command(TARGET GlintSpotter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/dist"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:GlintSpotter>" "${CMAKE_SOURCE_DIR}/dist/"
    COMMENT "Staging dist/"
    VERBATIM
)
