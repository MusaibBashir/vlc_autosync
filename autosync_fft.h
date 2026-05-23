#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Returns shift in seconds.
 * Positive  → subtitles are early  (delay them)
 * Negative  → subtitles are late   (advance them)
 *
 * audio_energy : RMS energy per bin, length n_bins
 * sub_activity : 1.0 if sub active in bin, else 0.0, length n_bins
 *
 * SPEED NOTE: both arrays should already be binarised by the caller so the
 * FFT operates on {0,1} integer-valued floats — this keeps the signal sparse
 * and makes the cross-correlation peak very clean.
 */
double autosync_compute_shift(
    const float *audio_energy,
    const float *sub_activity,
    size_t       n_bins,
    double       bin_size_sec
);
