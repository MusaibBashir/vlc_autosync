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
# Ubuntu ships libvlc-dev (public C API) and libvlccore-dev (internal plugin
# API, needed for vlc_common.h / vlc_plugin.h / vlc_filter.h etc.).
# Only "libvlc" has a pkg-config .pc file; "vlccore" does NOT — pkg_check_modules
# with REQUIRED vlccore therefore always fails on Ubuntu.
#
# Strategy:
#   1. Use pkg-config for libvlc  → gives us -I and -L for the public API.
#   2. Find vlc_common.h manually → tells us where the plugin headers live.
#   3. Link only against libvlc (public) + libvlccore (found via find_library).
#      Plugin modules are loaded by VLC at runtime; we don't need to link the
#      full vlccore static archive.
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVLC REQUIRED libvlc)   # libvlc only — always present

# Find the plugin-API headers (vlc_common.h lives one level above vlc/)
find_path(VLC_PLUGIN_INCLUDE_DIR
    NAMES vlc/vlc.h
    HINTS
        ${LIBVLC_INCLUDE_DIRS}
        /usr/include
        /usr/local/include
)
if(NOT VLC_PLUGIN_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find vlc/vlc.h. Install libvlc-dev and libvlccore-dev:\n"
        "  sudo apt-get install libvlc-dev libvlccore-dev")
endif()

# The internal plugin headers (vlc_common.h, vlc_plugin.h, vlc_filter.h …)
# sit in the same include tree — just add the parent so #include <vlc_common.h>
# resolves correctly.
set(VLC_INCLUDE_DIRS "${VLC_PLUGIN_INCLUDE_DIR}")

# Find libvlccore.so (needed at link time for vlc_object_*, msg_Info, etc.)
find_library(LIBVLCCORE_LIB
    NAMES vlccore
    HINTS ${LIBVLC_LIBRARY_DIRS} /usr/lib /usr/lib/x86_64-linux-gnu
)
if(NOT LIBVLCCORE_LIB)
    message(FATAL_ERROR
        "Could not find libvlccore.so. Install libvlccore-dev:\n"
        "  sudo apt-get install libvlccore-dev")
endif()

message(STATUS "VLC include dir : ${VLC_INCLUDE_DIRS}")
message(STATUS "libvlc          : ${LIBVLC_LIBRARIES}")
message(STATUS "libvlccore      : ${LIBVLCCORE_LIB}")

# ── kiss_fft (bundled) ───────────────────────────────────────────────────────
add_library(kissfft STATIC
    kiss_fft/kiss_fft.c
)
target_include_directories(kissfft PUBLIC kiss_fft)
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
    ${VLC_INCLUDE_DIRS}
    kiss_fft
)

target_link_libraries(autosync PRIVATE
    kissfft
    ${LIBVLC_LIBRARIES}     # -lvlc
    ${LIBVLCCORE_LIB}       # -lvlccore  (full path from find_library)
    m                       # libm for sqrt()
)

# Suppress the default "lib" prefix — we add it explicitly so the name is
# exactly libautosync_plugin.so as VLC expects.
set_target_properties(autosync PROPERTIES
    PREFIX      "lib"
    SUFFIX      "_plugin.so"
    OUTPUT_NAME "autosync"
)

# ── Install destination ───────────────────────────────────────────────────────
# Accept -DVLC_PLUGIN_DIR=... from the command line (CI passes it).
# If not given, derive from pkg-config libdir, then fall back to a hard path.
if(NOT DEFINED VLC_PLUGIN_DIR)
    if(LIBVLC_LIBDIR)
        set(VLC_PLUGIN_DIR "${LIBVLC_LIBDIR}/vlc/plugins")
    else()
        set(VLC_PLUGIN_DIR "/usr/lib/vlc/plugins")
    endif()
endif()

install(TARGETS autosync DESTINATION "${VLC_PLUGIN_DIR}/video_filter")
message(STATUS "Plugin install dir: ${VLC_PLUGIN_DIR}/video_filter")