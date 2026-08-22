cmake_minimum_required(VERSION 3.20)

# Read version automatically from version.json
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/version.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.json" VER_JSON_CONTENT)
    string(REGEX MATCH "\"version\"[ \t]*:[ \t]*\"([^\"]+)\"" _ "${VER_JSON_CONTENT}")
    if(CMAKE_MATCH_1)
        set(APP_VERSION "${CMAKE_MATCH_1}")
    else()
        set(APP_VERSION "1.2.1")
    endif()
else()
    set(APP_VERSION "1.2.1")
endif()

message(STATUS "Building Motion CLI Version: ${APP_VERSION} (read from version.json)")

project(motioncli
    VERSION ${APP_VERSION}
    DESCRIPTION "Super-lightweight live wallpaper CLI - Dev by tenzo"
    LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "Motion CLI targets Windows only (uses WorkerW + Media Foundation).")
endif()

file(GLOB_RECURSE MOTION_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")

# Core portable application
add_executable(motioncli WIN32 ${MOTION_SOURCES} "${CMAKE_SOURCE_DIR}/resources/app.rc")

target_include_directories(motioncli PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/resources")

target_compile_definitions(motioncli PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    MOTION_VERSION="${APP_VERSION}")

if(MSVC)
    target_compile_options(motioncli PRIVATE /W3 /utf-8 /EHsc)
    target_compile_options(motioncli PRIVATE $<$<CONFIG:Release>:/GL /O2 /Oy2 /Zc:inline /Qpar>)
    target_link_options(motioncli PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
endif()

target_link_libraries(motioncli PRIVATE
    winhttp mf mfplat mfuuid mfreadwrite shlwapi ole32 shell32 comdlg32 dxgi d3d11 windowscodecs)

# Terminal-based Interactive Web Installer (Reads version.json directly from GitHub)
add_executable(MotionCLI-Installer
    "${CMAKE_SOURCE_DIR}/installer/installer_main.cpp"
    "${CMAKE_SOURCE_DIR}/installer/installer.rc")

target_compile_definitions(MotionCLI-Installer PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    MOTION_VERSION="${APP_VERSION}")

if(MSVC)
    target_compile_options(MotionCLI-Installer PRIVATE /W3 /utf-8 /EHsc)
    target_compile_options(MotionCLI-Installer PRIVATE $<$<CONFIG:Release>:/O2 /Oy2 /Zc:inline>)
endif()

target_link_libraries(MotionCLI-Installer PRIVATE
    winhttp shell32 ole32 shlwapi advapi32)
