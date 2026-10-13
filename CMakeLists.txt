cmake_minimum_required(VERSION 3.20)

# Pin the minimum macOS version rather than letting it default.
#
# With this unset, clang targets whatever the machine doing the build happens
# to be running (clamped to the SDK), so the floor moved on its own whenever
# the build machine was updated: 0.9.0 shipped requiring 26.0, and the first
# build after an OS update silently produced a binary demanding 26.5 -- cutting
# off every user between the two for no reason anyone chose.
#
# Set before project() deliberately: CMake bakes this into the compiler checks
# it runs there, and a value arriving afterwards only reaches some of the
# flags. 26.0 is what is already in the wild (and what the vendored dylibs are
# built against) -- raise it on purpose, not by upgrading a laptop.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING
        "Minimum macOS version the build targets" FORCE)
endif()

project(SinkTerminal VERSION 0.10.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One source of truth for the version: the project() line above. It used to be
# repeated in main.cpp's SDL metadata and in releases/package.sh's Info.plist,
# which is how the plist ended up saying 0.8 while three other places moved on.
# releases/package.sh reads the same project() line rather than carrying a
# fourth copy.
#
# SINK_VERSION_NUM is the flat integer form secondary DA reports (CSI > c),
# where the convention is a bare patch level: 0.8.0 -> 800.
math(EXPR SINK_VERSION_NUM
     "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
add_compile_definitions(
    SINK_VERSION="${PROJECT_VERSION}"
    SINK_VERSION_NUM=${SINK_VERSION_NUM}
)

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
# zlib, for kitty graphics' o=z compression. The SDK's own; nothing to bundle.
find_package(ZLIB REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)

# Shell integration scripts (shell-integration/), embedded in the binary so
# the app installs exactly the version it was built with. Regenerated when a
# script changes.
set(SHELL_INTEGRATION_INC ${CMAKE_BINARY_DIR}/generated/shell_integration_scripts.inc)
set(SHELL_INTEGRATION_FILES
    ${CMAKE_SOURCE_DIR}/shell-integration/zsh/.zshenv
    ${CMAKE_SOURCE_DIR}/shell-integration/zsh/sink-integration.zsh
    ${CMAKE_SOURCE_DIR}/shell-integration/bash/sink.bash
)
add_custom_command(
    OUTPUT ${SHELL_INTEGRATION_INC}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/shell-integration
            -DOUT=${SHELL_INTEGRATION_INC}
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_shell_integration.cmake
    DEPENDS ${SHELL_INTEGRATION_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_shell_integration.cmake
    COMMENT "Embedding shell integration scripts"
)
# What a target needs to spawn shells (PTYBridge) with the integration.
set(SHELL_SPAWN_SOURCES
    src/pty_bridge.cpp
    src/private_files.cpp
    src/shell_integration.cpp
    ${SHELL_INTEGRATION_INC}
)

# Common Source Files
set(SOURCES
    src/main.cpp
    src/font_manager.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    ${SHELL_SPAWN_SOURCES}
    src/prompt_editing.cpp
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
target_include_directories(sink PRIVATE ${CMAKE_BINARY_DIR}/generated)

target_include_directories(sink PRIVATE src vendor/tomlplusplus)

target_link_libraries(sink PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
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
    src/private_files.cpp
    src/shell_integration.cpp
    ${SHELL_INTEGRATION_INC}
    src/prompt_editing.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    src/font_manager.cpp
)
target_include_directories(sink_tests PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(sink_tests PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)
add_test(NAME ansi_parser COMMAND sink_tests)

# Mouse editing of the command line, end to end. Runs a real zsh in a PTY,
# renders it through the parser and grid, clicks through prompt_edit -- the
# code main.cpp calls -- and asks the shell where its cursor ended up. Never
# opens a window or touches the mouse, so it runs while the machine is in use.
# Labelled "integration" since it spawns shells and depends on their timing.
add_executable(prompt_editing_e2e
    tests/prompt_editing_e2e.cpp
    src/private_files.cpp
    src/shell_integration.cpp
    ${SHELL_INTEGRATION_INC}
    src/prompt_editing.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    src/font_manager.cpp
)
target_include_directories(prompt_editing_e2e PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(prompt_editing_e2e PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)
add_test(NAME prompt_editing_zsh COMMAND prompt_editing_e2e --shell zsh)
# bash 5 from Homebrew; the binary reports a skip and passes when it is absent
# (macOS's own bash 3.2 cannot report its cursor to the test).
add_test(NAME prompt_editing_bash COMMAND prompt_editing_e2e --shell bash)
# The same, with each shell started the way sink starts it by default: with
# shell integration, against a scratch HOME of test dotfiles. Adds checks only
# integration makes possible -- startup files loading exactly as without sink,
# typing ahead of the prompt, autosuggestions, the reported directory.
add_test(NAME prompt_editing_zsh_integration COMMAND prompt_editing_e2e --shell zsh --integration)
add_test(NAME prompt_editing_bash_integration COMMAND prompt_editing_e2e --shell bash --integration)
set_tests_properties(prompt_editing_zsh prompt_editing_bash
                     prompt_editing_zsh_integration prompt_editing_bash_integration
                     PROPERTIES LABELS integration TIMEOUT 900)

# tmux compatibility proof. Drives a real tmux in a PTY, replays what it writes
# through sink's parser, and compares against tmux's own view. Registered only
# when tmux is installed, and labelled "integration" because it spawns servers
# and depends on timing -- exclude with `ctest -LE integration` if that becomes
# a problem in CI.
find_program(TMUX_EXECUTABLE tmux)
if(TMUX_EXECUTABLE)
    add_test(NAME tmux_compat
             COMMAND ${CMAKE_COMMAND} -E env
                     python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/tmux_compat.py
                     $<TARGET_FILE:tmux_replay>
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(tmux_compat PROPERTIES LABELS integration TIMEOUT 180)
else()
    message(STATUS "tmux not found; skipping the tmux compatibility test")
endif()

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
    src/terminal_images.cpp
    src/font_manager.cpp
)
target_include_directories(sink_bench PRIVATE src)
target_link_libraries(sink_bench PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)

# Replays a captured terminal byte stream through the parser and prints the
# resulting screen. Used by tests/tmux_compat.py to compare sink's rendering of
# tmux's output against tmux's own view of what it drew.
add_executable(tmux_replay
    tests/tmux_replay.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    src/font_manager.cpp
)
target_include_directories(tmux_replay PRIVATE src)
target_link_libraries(tmux_replay PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)

# Frame-time benchmark for the render path, which sink_bench (headless by
# design) structurally cannot measure. Needs a real renderer and glyph atlas,
# so it opens a hidden window.
add_executable(pty_bench
    bench/pty_bench.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    src/font_manager.cpp
    ${SHELL_SPAWN_SOURCES}
)
target_include_directories(pty_bench PRIVATE src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(pty_bench PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)

add_executable(render_bench
    bench/render_bench.cpp
    src/ansi_parser.cpp
    src/terminal_grid.cpp
    src/terminal_images.cpp
    src/font_manager.cpp
)
target_include_directories(render_bench PRIVATE src)
target_link_libraries(render_bench PRIVATE
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
    SDL3_image::SDL3_image
    ZLIB::ZLIB
)
