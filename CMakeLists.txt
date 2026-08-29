cmake_minimum_required(VERSION 3.20)
project(SinkTerminal VERSION 0.8.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to an optimized build. With CMAKE_BUILD_TYPE unset CMake passes no
# -O flag at all, so every build -- including the one that gets packaged into
# sink.app -- was compiled at -O0. On the parser benchmark that costs between
# 2.5x (UTF-8) and 7x (cursor-heavy) throughput.
# Override for development with: cmake -B build -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Find SDL3, SDL3_ttf, and SDL3_image
# 3.4.0 minimum: the video background hue-shift effect uses the GPU render
# backend and SDL_GPURenderState custom-shader API, both added in 3.4.0.
find_package(SDL3 3.4.0 REQUIRED)
# Prefer the vendored SDL3_ttf + HarfBuzz (vendor/build_text_stack.sh).
# Homebrew's harfbuzz links glib (and thus libpcre2 + libintl) and graphite2:
# ~2.2MB for a Unicode backend harfbuzz has built in, and a font technology
# (SIL Graphite) sink never loads.
set(SINK_VENDORED_TEXT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/textstack")
if(EXISTS "${SINK_VENDORED_TEXT}/lib/cmake/SDL3_ttf/SDL3_ttfConfig.cmake")
    message(STATUS "Using vendored SDL3_ttf + HarfBuzz: ${SINK_VENDORED_TEXT}")
    list(PREPEND CMAKE_PREFIX_PATH "${SINK_VENDORED_TEXT}")
else()
    message(WARNING
        "Vendored SDL3_ttf not found. Falling back to the system one, which "
        "pulls in glib/graphite2. Run vendor/build_text_stack.sh.")
endif()
find_package(SDL3_ttf REQUIRED)
# Prefer the vendored SVG-only SDL3_image (vendor/build_sdl3_image.sh).
# Homebrew's enables every format, pulling ~12MB of AVIF/JXL/TIFF/WebP decoders
# and their deps into the bundle for what amounts to two SVG logos.
set(SINK_VENDORED_SDL3_IMAGE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sdl3_image")
if(EXISTS "${SINK_VENDORED_SDL3_IMAGE}/lib/cmake/SDL3_image/SDL3_imageConfig.cmake")
    message(STATUS "Using vendored SDL3_image: ${SINK_VENDORED_SDL3_IMAGE}")
    list(PREPEND CMAKE_PREFIX_PATH "${SINK_VENDORED_SDL3_IMAGE}")
else()
    message(WARNING
        "Vendored SDL3_image not found. Falling back to the system one, which "
        "bundles ~12MB of unused image codecs. Run vendor/build_sdl3_image.sh.")
endif()
find_package(SDL3_image REQUIRED)

# Prefer the vendored LGPL decode-only FFmpeg (built by vendor/build_ffmpeg.sh).
# Homebrew's ffmpeg is configured --enable-gpl --enable-version3, so bundling it
# into sink.app would relicense the app as GPLv3 and drag in ~33MB of encoders
# sink never calls. Fall back to the system one for development, but warn.
set(SINK_VENDORED_FFMPEG "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ffmpeg")
if(EXISTS "${SINK_VENDORED_FFMPEG}/lib/pkgconfig/libavcodec.pc")
    message(STATUS "Using vendored LGPL FFmpeg: ${SINK_VENDORED_FFMPEG}")
    set(ENV{PKG_CONFIG_PATH} "${SINK_VENDORED_FFMPEG}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
else()
    message(WARNING
        "Vendored FFmpeg not found. Falling back to the system FFmpeg, which is "
        "almost certainly GPL and must not be shipped. Run vendor/build_ffmpeg.sh "
        "before packaging a release.")
endif()

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
    src/sink_demo.cpp
    src/preset_manager.cpp
    src/hue_shift.cpp
    src/crt_shader.cpp
)

# macOS Cocoa Menu Integration
if(APPLE)
    list(APPEND SOURCES src/macos_menu.mm)
endif()

# Renamed executable target to "sink"
add_executable(sink ${SOURCES})

target_include_directories(sink PRIVATE src vendor/tomlplusplus)

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

# Parser/grid unit tests: no window or PTY, so only the sources the grid
# pulls in (font_manager for the render path's symbols) are needed.
enable_testing()
add_executable(sink_tests
    tests/test_ansi_parser.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/font_manager.cpp
)
target_include_directories(sink_tests PRIVATE src)
target_link_libraries(sink_tests PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
)
add_test(NAME ansi_parser COMMAND sink_tests)

# Config store tests: no window/PTY/renderer needed, just the TOML-backed
# preset store, so it's a separate lightweight binary rather than piling
# onto sink_tests (which already owns its own main()).
add_executable(preset_manager_tests
    tests/test_preset_manager.cpp
    src/preset_manager.cpp
)
target_include_directories(preset_manager_tests PRIVATE src vendor/tomlplusplus)
add_test(NAME preset_manager COMMAND preset_manager_tests)

# Headless parser/grid throughput benchmark (see bench/README.md). Not part
# of `ctest` -- it prints a timing table for humans, not pass/fail checks.
add_executable(sink_bench
    bench/sink_bench.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/font_manager.cpp
)
target_include_directories(sink_bench PRIVATE src)
target_link_libraries(sink_bench PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
)
