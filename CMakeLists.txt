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
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local"
        CACHE PATH "Install prefix (default: user-local)" FORCE)
endif()

add_executable(game-of-life
    src/main.c
    src/board.c
    src/config.c
    src/engine.c
    src/history.c
    src/kitty.c
    src/popup.c
    src/rle.c
    src/settings.c
    src/sixel.c
    src/sparse.c
    src/terminal.c)

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

# ---- Test: KGP canvas + sixel fallback -------------------------------------
enable_testing()
add_executable(test-kitty
    tests/test_kitty.c
    src/kitty.c
    src/sixel.c
    src/board.c)
target_include_directories(test-kitty PRIVATE src)
target_link_libraries(test-kitty PRIVATE ZLIB::ZLIB)
target_compile_options(test-kitty PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>)
add_test(NAME kitty-canvas COMMAND test-kitty)

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

# Install the bundled patterns (saves/*.rle, including the auto-loaded
# default.rle) into the XDG *data* location's saves directory — where the program
# looks when run without -f, and where the in-app Save/Load browses. Kept separate
# from the config directory (settings) on purpose. Evaluated at install time so it
# respects the invoking user's HOME / XDG_DATA_HOME, and it never clobbers a
# pattern the user already has (their own edits/saves are safe).
file(GLOB GOL_BUNDLED_PATTERNS
     RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/saves"
     "${CMAKE_CURRENT_SOURCE_DIR}/saves/*.rle")
install(CODE "
    set(data_home \"\$ENV{XDG_DATA_HOME}\")
    if(data_home STREQUAL \"\")
        set(data_home \"\$ENV{HOME}/.local/share\")
    endif()
    set(saves \"\${data_home}/game-of-life/saves\")
    foreach(pat ${GOL_BUNDLED_PATTERNS})
        set(dest \"\${saves}/\${pat}\")
        if(EXISTS \"\${dest}\")
            message(STATUS \"Keeping existing pattern: \${dest}\")
        else()
            file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/saves/\${pat}\"
                 DESTINATION \"\${saves}\")
            message(STATUS \"Installed pattern: \${dest}\")
        endif()
    endforeach()
")

# --- Uninstall (removes the executable, bundled patterns, settings) ---------
# Bake the shipped pattern names in so uninstall removes exactly those and never
# the user's own saved patterns.
list(JOIN GOL_BUNDLED_PATTERNS " " GOL_BUNDLED_PATTERNS_STR)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    COMMENT "Uninstalling game-of-life and its default config")
