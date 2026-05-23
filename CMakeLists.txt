cmake_minimum_required(VERSION 3.16)
project(vlc_autosync C)

set(CMAKE_C_STANDARD 11)

# ── Compiler optimisation flags ─────────────────────────────────────────────
# SPEED FIX: The original had no optimisation flags, so the code compiled at
# -O0 (debug).  FFT and inner PCM loops are the hot path; -O2 gives a
# free 3-5× speedup on them with no code changes.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Release = -O2 -DNDEBUG on GCC/Clang; also enable -ffast-math for FFT.
# -ffast-math is safe here: we have no NaN/Inf sentinel values and don't
# need strict IEEE 754 ordering in the RMS / energy path.
add_compile_options(
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffast-math>
    $<$<CONFIG:Release>:-funroll-loops>
)

# ── Find VLC plugin headers ──────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(VLC REQUIRED libvlc vlccore)

# ── kiss_fft (bundled) ───────────────────────────────────────────────────────
add_library(kissfft STATIC
    kiss_fft/kiss_fft.c
)
target_include_directories(kissfft PUBLIC kiss_fft)

# FIX: Also apply optimisation to kissfft — its inner butterfly loop benefits
# enormously from -O2 -ffast-math (typically 4-8× faster than -O0).
target_compile_options(kissfft PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffast-math>
    $<$<CONFIG:Release>:-funroll-loops>
)

# ── Our plugin ───────────────────────────────────────────────────────────────
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
    ${VLC_LIBRARIES}
    m           # libm for sqrt()
)

# VLC plugins must be named lib<name>_plugin.so
set_target_properties(autosync PROPERTIES
    PREFIX      "lib"
    SUFFIX      "_plugin.so"
    OUTPUT_NAME "autosync"
)

# ── Install ───────────────────────────────────────────────────────────────────
# FIX: vlc-config --pluginsdir is deprecated on modern VLC; prefer pkg-config.
# Fall back to vlc-config only if pkg-config doesn't provide the path.
if(DEFINED VLC_LIBDIR)
    # pkg_check_modules sets VLC_LIBDIR; derive pluginsdir from it
    set(VLC_PLUGIN_DIR "${VLC_LIBDIR}/vlc/plugins")
else()
    execute_process(
        COMMAND         vlc-config --pluginsdir
        OUTPUT_VARIABLE VLC_PLUGIN_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT VLC_PLUGIN_DIR)
    message(WARNING "Could not determine VLC plugin directory; "
                    "set VLC_PLUGIN_DIR manually with -DVLC_PLUGIN_DIR=...")
    set(VLC_PLUGIN_DIR "/usr/lib/vlc/plugins")
endif()

install(TARGETS autosync DESTINATION "${VLC_PLUGIN_DIR}/video_filter")

message(STATUS "AutoSync plugin will install to: ${VLC_PLUGIN_DIR}/video_filter")
