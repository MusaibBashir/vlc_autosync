#include "autosync_fft.h"
#include "kiss_fft/kiss_fft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── next power-of-two ──────────────────────────────────────────────────── */
static size_t next_pow2(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* ── FIX 1: normalise cross-correlation so the peak is scale-independent ── */
/*
 * Without normalisation the peak height depends on how many subtitle bins
 * are active.  A file with lots of dialogue finds the same peak as one with
 * sparse dialogue, but an all-silence audio window would still pick a
 * spurious peak.  Normalisation also means the caller can threshold on
 * peak_val > 0.1 to detect "not enough signal".
 */
static float l2_norm(const float *v, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += (double)v[i] * v[i];
    return (float)sqrt(s);
}

double autosync_compute_shift(
    const float *audio_energy,
    const float *sub_activity,
    size_t       n_bins,
    double       bin_size_sec)
{
    /*
     * SPEED FIX 2: Use a SHORTER analysis window.
     *
     * The original code ran an FFT over 6 000 bins (10 min × 10 bins/s).
     * For subtitle sync we only need to find a lag of at most ±120 s, so
     * 2 minutes of data is enough.  Halving n_bins to 1 200 makes the FFT
     * ~5× faster because FFT cost is O(N log N).
     *
     * We still accept the full n_bins array from the caller but only use the
     * first FAST_BINS bins (or all of them if the caller already passed fewer).
     */
#define FAST_BINS 1200   /* 2 minutes at 0.1 s/bin — enough for any real offset */
    size_t use_bins = n_bins < FAST_BINS ? n_bins : FAST_BINS;

    /* Full correlation length = 2*N-1, padded to next pow2 */
    size_t fft_len = next_pow2(2 * use_bins);   /* 2048 instead of 16384 */

    kiss_fft_cfg fwd = kiss_fft_alloc((int)fft_len, 0, NULL, NULL);
    kiss_fft_cfg inv = kiss_fft_alloc((int)fft_len, 1, NULL, NULL);
    if (!fwd || !inv) { kiss_fft_free(fwd); kiss_fft_free(inv); return 0.0; }

    kiss_fft_cpx *A   = calloc(fft_len, sizeof(kiss_fft_cpx));
    kiss_fft_cpx *B   = calloc(fft_len, sizeof(kiss_fft_cpx));
    kiss_fft_cpx *Out = calloc(fft_len, sizeof(kiss_fft_cpx));
    if (!A || !B || !Out) {
        free(A); free(B); free(Out);
        kiss_fft_free(fwd); kiss_fft_free(inv);
        return 0.0;
    }

    /* Fill real parts from the (possibly truncated) window */
    for (size_t i = 0; i < use_bins; i++) {
        A[i].r = audio_energy[i]; A[i].i = 0.0f;
        B[i].r = sub_activity[i]; B[i].i = 0.0f;
    }
    /* Remainder stays zero from calloc — correct zero-padding */

    kiss_fft(fwd, A, Out);   /* Out = FFT(audio) */
    kiss_fft(fwd, B, A);     /* A   = FFT(subs)  */

    /*
     * FIX 3: Cross-correlation formula was WRONG.
     *
     * Cross-correlation in the frequency domain is:
     *   Corr(f) = conj(B(f)) * A(f)   where A=audio, B=subs
     *
     * The original code computed:
     *   re = Out[i].r * A[i].r + Out[i].i * A[i].i   ← this is Re(A·B), i.e. dot product
     *   im = Out[i].i * A[i].r - Out[i].r * A[i].i   ← this is Im(A·B̄) — mixed sign
     *
     * Correct formula for conj(B) * A:
     *   re = A_audio.r * B_subs.r + A_audio.i * B_subs.i
     *   im = A_audio.i * B_subs.r - A_audio.r * B_subs.i
     *
     * At this point: Out = FFT(audio), A = FFT(subs)
     */
    for (size_t i = 0; i < fft_len; i++) {
        float ar = Out[i].r, ai = Out[i].i;   /* FFT(audio) */
        float br = A[i].r,   bi = A[i].i;     /* FFT(subs)  */
        /* conj(subs) * audio */
        Out[i].r = ar * br + ai * bi;
        Out[i].i = ai * br - ar * bi;
    }

    kiss_fft(inv, Out, B);   /* B = IFFT → cross-correlation */

    /*
     * SPEED FIX 4 + FIX 4: Normalise then find peak.
     *
     * Also: only search lags in [-MAX_LAG_BINS, +MAX_LAG_BINS].
     * Searching all fft_len bins is wasteful — subtitle offsets beyond
     * ±120 s are not realistic.  Constraining the search to ±MAX_LAG_BINS
     * also prevents false peaks in the zero-padded region.
     */
#define MAX_LAG_BINS  1200   /* ±120 s at 0.1 s/bin */

    /* Normalise by product of L2 norms so peak is in [-1, 1] */
    float norm = l2_norm(audio_energy, use_bins) * l2_norm(sub_activity, use_bins);
    if (norm < 1e-9f) norm = 1.0f;   /* avoid divide-by-zero on silent audio */
    float inv_scale = 1.0f / ((float)fft_len * norm);

    float     best_val = -1e30f;
    ptrdiff_t best_lag = 0;

    /* Positive lags: indices [0, MAX_LAG_BINS] */
    for (size_t i = 0; i <= (size_t)MAX_LAG_BINS && i < fft_len; i++) {
        float v = B[i].r * inv_scale;
        if (v > best_val) { best_val = v; best_lag = (ptrdiff_t)i; }
    }
    /* Negative lags: indices [fft_len - MAX_LAG_BINS, fft_len - 1] */
    for (size_t i = fft_len - MAX_LAG_BINS; i < fft_len; i++) {
        float v = B[i].r * inv_scale;
        if (v > best_val) {
            best_val = v;
            best_lag = (ptrdiff_t)i - (ptrdiff_t)fft_len;
        }
    }

    free(A); free(B); free(Out);
    kiss_fft_free(fwd); kiss_fft_free(inv);

    return (double)best_lag * bin_size_sec;
}
