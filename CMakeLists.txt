cmake_minimum_required(VERSION 3.20)
project(SinkTerminal CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find SDL3, SDL3_ttf, and SDL3_image
find_package(SDL3 REQUIRED)
find_package(SDL3_ttf REQUIRED)
find_package(SDL3_image REQUIRED)

# Find FFmpeg using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

# Common Source Files
set(SOURCES
    src/main.cpp
    src/font_manager.cpp
    src/terminal_grid.cpp
    src/pty_bridge.cpp
    src/ansi_parser.cpp
    src/video_engine.cpp
    src/settings_ui.cpp
)

# macOS Cocoa Menu Integration
if(APPLE)
    list(APPEND SOURCES src/macos_menu.mm)
endif()

# Renamed executable target to "sink"
add_executable(sink ${SOURCES})

target_include_directories(sink PRIVATE src)

target_link_libraries(sink PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
)

# macOS Frameworks
if(APPLE)
    target_link_libraries(sink PRIVATE "-framework Cocoa")
endif()

# Linux compatibility: link against libutil for forkpty
if(NOT APPLE AND NOT WIN32)
    find_library(UTIL_LIBRARY util)
    if(UTIL_LIBRARY)
        target_link_libraries(sink PRIVATE ${UTIL_LIBRARY})
    endif()
endif()
