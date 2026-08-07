# NukeEngine-Editor — CMake build for NON-Windows platforms (Deuterium).
# On Windows the editor stays in the hand-maintained NukeEngine-Editor.vcxproj driven by
# NukeEngine.sln (nukegen prebuild, vcpkg autolink, resource icon). This file builds the
# same sources against the NukeEngine + NukeImGui CMake targets of the superbuild.
cmake_minimum_required(VERSION 3.20)
project(NukeEngineEditor CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/../NukeEngine/cmake/NukeRunDir.cmake")   # NUKE_RUN_SUBDIR: x64 / macos / linux

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    message(STATUS "NukeEngine-Editor: built by NukeEngine.sln on Windows — CMake path skipped")
    return()
endif()

set(NUKE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

file(GLOB EDITOR_PANELS   "${CMAKE_CURRENT_SOURCE_DIR}/src/panels/*.cpp")
file(GLOB EDITOR_TEXTEDIT "${CMAKE_CURRENT_SOURCE_DIR}/src/textedit/*.cpp")

add_executable(NukeEngine-Editor
    main.cpp
    ${EDITOR_PANELS}
    ${EDITOR_TEXTEDIT}
    src/platform/platform_win32.cpp   # fully #ifdef _WIN32 — compiles empty here
    src/platform/platform_other.cpp   # Linux stubs — empty on macOS
)
if(APPLE)
    target_sources(NukeEngine-Editor PRIVATE src/platform/platform_macos.mm)
    target_link_libraries(NukeEngine-Editor PRIVATE
        "-framework AppKit" "-framework CoreServices")
    # The bundle built HERE is thin: binary + Info.plist + Resources. The superbuild's final
    # NukeEditorBundle target (root CMakeLists) then mirrors the runtime (player, dylibs,
    # modules/, shaders/, fonts/) into Contents/MacOS, making the .app self-contained for
    # /Applications. In the dev tree RunRoot() still prefers the loose layout beside it.
    # Arch stamp for the bundle plist (standalone configure: compute what the superbuild
    # would have — see the root CMakeLists).
    if(NOT DEFINED NUKE_BUILD_ARCHS)
        if(CMAKE_OSX_ARCHITECTURES)
            string(REPLACE ";" "+" NUKE_BUILD_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(NUKE_BUILD_ARCHS "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
    endif()
    target_sources(NukeEngine-Editor PRIVATE res/logo.icns dist/imgui.ini)
    set_source_files_properties(res/logo.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)   # Dock/Finder icon (CFBundleIconFile in the plist)
    set_source_files_properties(dist/imgui.ini PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources
        HEADER_FILE_ONLY ON)   # stock DOCKED layout: the first-run seed for installed editors
    set_target_properties(NukeEngine-Editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
endif()

target_compile_definitions(NukeEngine-Editor PRIVATE
    EDITOR
    GLM_ENABLE_EXPERIMENTAL
    _USE_MATH_DEFINES)
# Baked as the first VCPKG_ROOT probe for GUI launches (no login-shell env); a foreign
# machine skips the stale path and falls through to the common homes.
if(NUKE_VCPKG_ROOT)
    target_compile_definitions(NukeEngine-Editor PRIVATE "NUKE_VCPKG_ROOT_DEF=\"${NUKE_VCPKG_ROOT}\"")
endif()

target_include_directories(NukeEngine-Editor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/deps
    ${NUKE_ROOT}/NukeImGui/imgui
    ${NUKE_ROOT}/NukeImGui/src
    ${NUKE_ROOT}/NukeImGui/imguizmo)

# Engine target carries boost/glm/nlohmann PUBLIC; assimp + stb are direct editor uses
# (model preview import, icon/screenshot IO).
find_package(assimp CONFIG REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)
target_include_directories(NukeEngine-Editor PRIVATE ${STB_INCLUDE_DIRS})

target_link_libraries(NukeEngine-Editor PRIVATE
    NukeEngine
    NukeImGui
    assimp::assimp)

target_compile_options(NukeEngine-Editor PRIVATE -Wno-c++11-narrowing -Wno-unknown-attributes)

# Run dir FIRST, bundle second: dev loads the loose fresh dylibs even though the superbuild
# mirrors copies into the bundle; a standalone .app falls through to @loader_path.
set_target_properties(NukeEngine-Editor PROPERTIES
    BUILD_RPATH "@loader_path/../../..;@loader_path;$ORIGIN")

# Deploy into the run dir next to the engine + modules (same layout as Windows).
set(RUN_DIR "${NUKE_ROOT}/NukeEngine/${NUKE_RUN_SUBDIR}/$<CONFIG>")
if(APPLE)
    add_custom_command(TARGET NukeEngine-Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RUN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "$<TARGET_BUNDLE_DIR:NukeEngine-Editor>" "${RUN_DIR}/NukeEngine-Editor.app"
        # dist/ = the editor's stock run-dir files — imgui.ini carries the DEFAULT DOCKED
        # layout (without it every panel spawns floating and NoAutoMerge detaches them all).
        # copy_directory_if_different mirrors the vcxproj's `xcopy /d`: a user-modified
        # newer imgui.ini in the run dir is left alone.
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${RUN_DIR}"
        COMMENT "Deploying NukeEngine-Editor.app + stock run-dir files (dist/)")
else()
    add_custom_command(TARGET NukeEngine-Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RUN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:NukeEngine-Editor>" "${RUN_DIR}/"
        COMMENT "Deploying NukeEngine-Editor to the run dir")
endif()
