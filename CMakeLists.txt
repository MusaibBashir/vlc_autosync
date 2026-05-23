cmake_minimum_required(VERSION 3.16)
project(vlc_autosync C)

set(CMAKE_C_STANDARD 11)

# ── Build type & optimisation ────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffast-math>
    $<$<CONFIG:Release>:-funroll-loops>
)

# ── Find VLC headers ─────────────────────────────────────────────────────────
#
# Ubuntu package layout:
#   libvlc-dev     → /usr/include/vlc/vlc.h  (public API, has .pc file)
#   libvlccore-dev → /usr/include/vlc/plugins/vlc_common.h  (plugin API, NO .pc file)
#
# Our code does  #include <vlc_common.h>  (no path prefix), so we need
# -I/usr/include/vlc/plugins  on the compiler command line.
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVLC REQUIRED libvlc)

# pkg-config gives us -I/usr/include; plugin headers are one level deeper.
# Build a list of candidate paths from whatever pkg-config reported.
set(_VLC_PLUGIN_HEADER_HINTS)
foreach(_idir IN LISTS LIBVLC_INCLUDE_DIRS)
    list(APPEND _VLC_PLUGIN_HEADER_HINTS
        "${_idir}/vlc/plugins"
        "${_idir}/vlc"
        "${_idir}"
    )
endforeach()
# Also try the standard hard paths in case pkg-config gives nothing.
list(APPEND _VLC_PLUGIN_HEADER_HINTS
    /usr/include/vlc/plugins
    /usr/local/include/vlc/plugins
)

find_path(VLC_PLUGIN_INCLUDE_DIR
    NAMES vlc_common.h
    HINTS ${_VLC_PLUGIN_HEADER_HINTS}
    NO_DEFAULT_PATH
)

if(NOT VLC_PLUGIN_INCLUDE_DIR)
    message(FATAL_ERROR
        "Cannot find vlc_common.h.\n"
        "Make sure libvlccore-dev is installed:\n"
        "  sudo apt-get install libvlc-dev libvlccore-dev\n"
        "Searched: ${_VLC_PLUGIN_HEADER_HINTS}")
endif()
message(STATUS "VLC plugin headers : ${VLC_PLUGIN_INCLUDE_DIR}")

# Find libvlccore.so for linking (no .pc file, use find_library).
find_library(LIBVLCCORE_LIB
    NAMES vlccore
    HINTS ${LIBVLC_LIBRARY_DIRS}
          /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
)
if(NOT LIBVLCCORE_LIB)
    message(FATAL_ERROR
        "Cannot find libvlccore.so.\n"
        "Install libvlccore-dev:  sudo apt-get install libvlccore-dev")
endif()
message(STATUS "libvlc             : ${LIBVLC_LIBRARIES}")
message(STATUS "libvlccore         : ${LIBVLCCORE_LIB}")

# ── kiss_fft (bundled) ───────────────────────────────────────────────────────
add_library(kissfft STATIC
    kiss_fft/kiss_fft.c
)
# kiss_fft.c includes "_kiss_fft_guts.h" with a relative path, so the include
# dir must be the folder that contains both files — i.e. kiss_fft/ itself.
target_include_directories(kissfft PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/kiss_fft
)
target_compile_options(kissfft PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffast-math>
    $<$<CONFIG:Release>:-funroll-loops>
)

# ── Plugin target ────────────────────────────────────────────────────────────
add_library(autosync MODULE
    autosync.c
    autosync_fft.c
)

target_include_directories(autosync PRIVATE
    ${VLC_PLUGIN_INCLUDE_DIR}   # /usr/include/vlc/plugins  → vlc_common.h etc.
    ${LIBVLC_INCLUDE_DIRS}      # /usr/include              → vlc/vlc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kiss_fft
)

target_link_libraries(autosync PRIVATE
    kissfft
    ${LIBVLC_LIBRARIES}
    ${LIBVLCCORE_LIB}
    m
)

set_target_properties(autosync PROPERTIES
    PREFIX      "lib"
    SUFFIX      "_plugin.so"
    OUTPUT_NAME "autosync"
)

# ── Install destination ───────────────────────────────────────────────────────
if(NOT DEFINED VLC_PLUGIN_DIR)
    if(LIBVLC_LIBDIR)
        set(VLC_PLUGIN_DIR "${LIBVLC_LIBDIR}/vlc/plugins")
    else()
        set(VLC_PLUGIN_DIR "/usr/lib/vlc/plugins")
    endif()
endif()

install(TARGETS autosync DESTINATION "${VLC_PLUGIN_DIR}/video_filter")
message(STATUS "Plugin install dir : ${VLC_PLUGIN_DIR}/video_filter")