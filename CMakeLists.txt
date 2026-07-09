cmake_minimum_required(VERSION 3.20)

project(game-of-life
    VERSION 1.0.0
    DESCRIPTION "Conway's Game of Life terminal program"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Default to Debug if no build type is specified (single-config generators).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Install to the user's home by default (~/.local) so `make install` needs no
# root. Override with -DCMAKE_INSTALL_PREFIX=... for a system-wide install.
# gol_real_home (see cmake/RealUserHome.cmake) resolves the invoking user's
# real home even if this very configure step is itself run under `sudo`
# (rare, but if it happens the plain $ENV{HOME} below would default the whole
# install under /root instead of the person who will actually run the game).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RealUserHome.cmake")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    gol_real_home(GOL_REAL_HOME)
    set(CMAKE_INSTALL_PREFIX "${GOL_REAL_HOME}/.local"
        CACHE PATH "Install prefix (default: user-local)" FORCE)
endif()

# Sources are grouped by responsibility:
#   src/core    — the world model (engine seam, sparse world, history, seed board)
#   src/render  — pixel-graphics encoders (Kitty Graphics Protocol, sixel)
#   src/io      — file formats + persistence (RLE, .cells, settings)
#   src/ui      — the terminal app (main loop, raw-mode terminal, popup toast)
add_executable(game-of-life
    src/core/board.c
    src/core/engine.c
    src/core/history.c
    src/core/sparse.c
    src/io/config.c
    src/io/rle.c
    src/io/settings.c
    src/render/kitty.c
    src/render/sixel.c
    src/ui/main.c
    src/ui/popup.c
    src/ui/terminal.c)

target_include_directories(game-of-life PRIVATE src)

# Optional multi-core stepping. When OpenMP is available, sparse_step fans the
# neighbour tally across cores (see src/sparse.c); without it the code compiles
# and runs single-threaded (guarded by #ifdef _OPENMP).
find_package(OpenMP)
if(OpenMP_C_FOUND)
    target_link_libraries(game-of-life PRIVATE OpenMP::OpenMP_C)
    message(STATUS "OpenMP found: multi-core stepping enabled")
else()
    message(STATUS "OpenMP not found: stepping stays single-threaded")
endif()

# Kitty Graphics Protocol encoder uses zlib. Both macOS and Linux ship it.
find_package(ZLIB REQUIRED)
target_link_libraries(game-of-life PRIVATE ZLIB::ZLIB)
message(STATUS "ZLIB found: KGP canvas enabled")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(game-of-life PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O2 -DNDEBUG>)
endif()

# --- Installation -----------------------------------------------------------
include(GNUInstallDirs)

# The executable goes to <prefix>/bin (i.e. ~/.local/bin by default).
install(TARGETS game-of-life RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Install the bundled patterns (saves/*.rle) via cmake/cmake_install.cmake.in,
# run at install time so it respects the invoking user's HOME / XDG_DATA_HOME.
# The shipped pattern names are baked into both the install and uninstall
# scripts so they act on exactly this set and never the user's own saves. The
# configured copy is named cmake_install_patterns.cmake because CMake itself
# generates <build>/cmake_install.cmake (the install manifest) and configuring
# over it would clobber the whole install step.
file(GLOB GOL_BUNDLED_PATTERNS
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/saves"
     "${CMAKE_CURRENT_SOURCE_DIR}/saves/*.rle")
list(JOIN GOL_BUNDLED_PATTERNS " " GOL_BUNDLED_PATTERNS_STR)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_install.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_install_patterns.cmake"
    @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/cmake_install_patterns.cmake")

# A named, explicit alias for the generator's built-in `install` target
# (which CMake reserves the name "install" for — add_custom_target(install ...)
# is a hard configure error on every generator, so this target has to be
# called something else). Both do exactly the same work; this one just gives
# `cmake --build <dir> --target install-game` a visible, self-documenting
# name in the CMakeLists.txt, mirroring `remove-game` below.
add_custom_target(install-game
    COMMAND ${CMAKE_COMMAND} --install "${CMAKE_CURRENT_BINARY_DIR}"
    COMMENT "Installing game-of-life (executable + bundled patterns)")

# --- Uninstall (removes the executable, bundled patterns, settings) ---------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    @ONLY)
add_custom_target(remove-game
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    COMMENT "Uninstalling game-of-life and its default config")
