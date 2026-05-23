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
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBVLC REQUIRED libvlc)

set(_VLC_PLUGIN_HEADER_HINTS)
foreach(_idir IN LISTS LIBVLC_INCLUDE_DIRS)
    list(APPEND _VLC_PLUGIN_HEADER_HINTS
        "${_idir}/vlc/plugins"
        "${_idir}/vlc"
        "${_idir}"
    )
endforeach()
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
    message(FATAL_ERROR "Cannot find vlc_common.h. Install libvlccore-dev.")
endif()

find_library(LIBVLCCORE_LIB
    NAMES vlccore
    HINTS ${LIBVLC_LIBRARY_DIRS}
          /usr/lib /usr/lib/x86_64-linux-gnu /usr/local/lib
)

if(NOT LIBVLCCORE_LIB)
    message(FATAL_ERROR "Cannot find libvlccore.so. Install libvlccore-dev.")
endif()

# ── kiss_fft (bundled) ───────────────────────────────────────────────────────
add_library(kissfft STATIC kiss_fft/kiss_fft.c)
target_include_directories(kissfft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/kiss_fft)
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

# REQUIRED FOR MODERN VLC: Inject the plugin name into the compiler
target_compile_definitions(autosync PRIVATE
    __PLUGIN__
    MODULE_STRING="autosync"
)

target_include_directories(autosync PRIVATE
    ${VLC_PLUGIN_INCLUDE_DIR}
    ${LIBVLC_INCLUDE_DIRS}
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