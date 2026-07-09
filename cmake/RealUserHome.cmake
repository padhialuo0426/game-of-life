# Resolve the "real" (non-root) user's home directory, even when the current
# process is running under `sudo`. Included both at CMake configure time (from
# CMakeLists.txt, for the default CMAKE_INSTALL_PREFIX) and at install/uninstall
# time (from the generated cmake_install_patterns.cmake / cmake_uninstall.cmake
# scripts, via an @CMAKE_CURRENT_SOURCE_DIR@-substituted include path).
#
# Why this exists: `sudo cmake --build <dir> --target install` is the documented
# way to do a system-wide install (see README "system-wide install"). Under
# sudo, $ENV{HOME} is root's home, not the invoking user's — so any script that
# blindly uses $ENV{HOME} to place per-user data (like the bundled patterns,
# or settings.json) ends up writing into /root, invisible to the actual user
# who will later run the program un-elevated. sudo sets SUDO_USER (and
# SUDO_UID/SUDO_GID) to the original account, so we can recover their real home
# via the OS's own `~user` shell expansion — portable across Linux and macOS,
# and correct even for non-conventional home paths (LDAP/NFS home dirs, etc.)
# that a naive "/home/$SUDO_USER" guess would get wrong.

# gol_real_home(<out-var>)
# Sets <out-var> to the invoking (non-root) user's home directory: the
# SUDO_USER-derived home when running under sudo, otherwise plain $ENV{HOME}.
# Falls back to $ENV{HOME} if SUDO_USER is set but its home can't be resolved
# (e.g. the account no longer exists), so a script can never regress below the
# pre-fix behaviour.
function(gol_real_home out_var)
    set(_home "$ENV{HOME}")
    set(_sudo_user "$ENV{SUDO_USER}")
    if(NOT _sudo_user STREQUAL "")
        # Ask the shell to expand ~username rather than guessing a path
        # convention: works for /home, /Users (macOS), and non-standard home
        # directories alike. The username is passed as a positional argument
        # ($1), never interpolated into the command string, so it is safe even
        # if a username somehow contained shell-special characters.
        execute_process(
            COMMAND sh -c "eval echo ~\"\$1\"" _ "${_sudo_user}"
            OUTPUT_VARIABLE _resolved
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _rc)
        if(_rc EQUAL 0 AND _resolved MATCHES "^/")
            set(_home "${_resolved}")
        else()
            message(WARNING
                "gol_real_home: could not resolve home for sudo user "
                "'${_sudo_user}', falling back to $ENV{HOME} (${_home})")
        endif()
    endif()
    set(${out_var} "${_home}" PARENT_SCOPE)
endfunction()
