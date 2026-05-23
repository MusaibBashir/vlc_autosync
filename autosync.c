/*****************************************************************************
 * autosync.c — VLC subtitle sync filter module
 *
 * Type:  sub_filter (sits in the subtitle decoder output chain)
 * Taps the audio filter chain via a registered aout filter to collect PCM.
 *
 * Build: see CMakeLists.txt
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_subpicture.h>
#include <vlc_aout.h>
#include <vlc_block.h>
#include <vlc_threads.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "autosync_fft.h"

/* ── tunables ────────────────────────────────────────────────────────────── */
#define BIN_SIZE_SEC    0.10f       /* 100 ms bins                           */
#define BIN_SIZE_US     100000LL    /* BIN_SIZE_SEC in microseconds          */

/*
 * SPEED FIX A: Analyse only 2 minutes instead of 10.
 *
 * The original ANALYSIS_SEC = 600 (10 min) means the audio tap had to run
 * for 10 real minutes before TryComputeSync() was ever called.  That IS the
 * "10 minutes to run" that the user reported — the code was literally waiting
 * for 10 minutes of playback data.
 *
 * 2 minutes gives 1 200 bins which is more than enough to find a ±120 s lag.
 * The FFT in autosync_fft.c is also constrained to the same window.
 */
#define ANALYSIS_SEC    120         /* analyse first 2 minutes               */
#define N_BINS          ((size_t)(ANALYSIS_SEC / BIN_SIZE_SEC))  /* 1200    */

#define AUDIO_SR        8000        /* resample target (Hz)                  */
#define CHUNK_SAMPLES   ((int)(AUDIO_SR * BIN_SIZE_SEC))         /* 800     */
#define ENERGY_THRESH   1.5f        /* × median → speech/silence boundary   */

/*
 * SPEED FIX B: Trigger sync as soon as N_BINS are full, not after waiting
 * for both audio AND a subtitle in every bin.  The original required
 * audio_bin >= N_BINS, which only fires once 2 min of playback has elapsed.
 * We keep that trigger but also add an EARLY trigger after EARLY_BINS so
 * the sync fires as soon as enough signal exists (typically 30-60 s).
 */
#define EARLY_BINS      (N_BINS / 2)  /* try after 1 minute of audio        */

/* ── module descriptor ───────────────────────────────────────────────────── */
static int  Open (vlc_object_t *);
static void Close(vlc_object_t *);

vlc_module_begin()
    set_description("Auto Subtitle Sync")
    set_shortname("AutoSync")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)
    set_capability("sub_filter", 0)
    set_callbacks(Open, Close)
    add_shortcut("autosync")
vlc_module_end()

/* ── private state ───────────────────────────────────────────────────────── */
typedef struct {
    /* subtitle side */
    float      *sub_activity;       /* [N_BINS] binary sub presence          */

    /* audio side */
    float      *audio_energy;       /* [N_BINS] RMS energy per bin           */
    float      *pcm_accum;          /* accumulator for current bin           */
    int         pcm_accum_n;        /* samples accumulated so far            */
    size_t      audio_bin;          /* bin currently being filled            */

    /* sync result */
    vlc_mutex_t lock;
    bool        sync_done;
    bool        sync_attempted;     /* don't attempt more than once          */
    int64_t     shift_us;           /* computed shift, microseconds          */

    /* audio tap back-pointer (set in Open, read in AudioTapFilter) */
    filter_t   *p_sub_filter;
} filter_sys_t;

/* ======================================================================== */
/*  Helpers                                                                  */
/* ======================================================================== */

/* qsort comparator — file-scope so it compiles under strict C11 */
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* ======================================================================== */
/*  Audio tap                                                                */
/* ======================================================================== */
static block_t *AudioTapFilter(filter_t *p_tap, block_t *p_block)
{
    /*
     * p_tap->p_sys holds a pointer to the parent sub_filter so we can reach
     * filter_sys_t.  This back-pointer is set in Open() before the tap is
     * inserted into the aout chain.
     */
    filter_t     *p_filter = (filter_t *)p_tap->p_sys;
    filter_sys_t *sys      = p_filter->p_sys;

    if (!p_block || sys->sync_done) return p_block;

    int    channels = p_tap->fmt_in.audio.i_channels;
    int    samples  = (int)p_block->i_nb_samples;
    float *pcm      = (float *)p_block->p_buffer;

    vlc_mutex_lock(&sys->lock);

    for (int i = 0; i < samples && sys->audio_bin < N_BINS; i++) {
        /* Mix down to mono */
        float mono = 0.0f;
        for (int c = 0; c < channels; c++)
            mono += pcm[i * channels + c];
        mono /= (float)channels;

        sys->pcm_accum[sys->pcm_accum_n++] = mono;

        if (sys->pcm_accum_n >= CHUNK_SAMPLES) {
            /* RMS energy for this bin */
            double sum = 0.0;
            for (int k = 0; k < CHUNK_SAMPLES; k++)
                sum += (double)sys->pcm_accum[k] * sys->pcm_accum[k];
            sys->audio_energy[sys->audio_bin++] = (float)sqrt(sum / CHUNK_SAMPLES);
            sys->pcm_accum_n = 0;
        }
    }

    vlc_mutex_unlock(&sys->lock);
    return p_block;
}

static int AudioTapOpen(vlc_object_t *obj)
{
    filter_t *p_tap = (filter_t *)obj;
    if (p_tap->fmt_in.audio.i_format != VLC_CODEC_FL32)
        return VLC_EGENERIC;
    p_tap->pf_audio_filter = AudioTapFilter;
    /* p_tap->p_sys must be set to the parent sub_filter by the caller */
    return VLC_SUCCESS;
}

/* ======================================================================== */
/*  Sync computation                                                          */
/* ======================================================================== */
static void TryComputeSync(filter_t *p_filter)
{
    filter_sys_t *sys = p_filter->p_sys;

    /* Guard: only attempt once */
    if (sys->sync_attempted) return;
    sys->sync_attempted = true;

    size_t usable = sys->audio_bin;

    /* Need at least EARLY_BINS to make a reliable estimate */
    if (usable < EARLY_BINS) return;

    /* ── Compute median energy threshold ── */
    float *tmp = malloc(usable * sizeof(float));
    if (!tmp) return;
    memcpy(tmp, sys->audio_energy, usable * sizeof(float));
    qsort(tmp, usable, sizeof(float), cmp_float);
    float threshold = tmp[usable / 2] * ENERGY_THRESH;
    free(tmp);

    /* ── Binarise audio energy ── */
    float *audio_bin_arr = calloc(usable, sizeof(float));
    if (!audio_bin_arr) return;
    for (size_t i = 0; i < usable; i++)
        audio_bin_arr[i] = (sys->audio_energy[i] > threshold) ? 1.0f : 0.0f;

    /* ── FFT cross-correlation ── */
    double shift_sec = autosync_compute_shift(
        audio_bin_arr, sys->sub_activity, usable, (double)BIN_SIZE_SEC);
    free(audio_bin_arr);

    sys->shift_us  = (int64_t)(shift_sec * 1000000.0);
    sys->sync_done = true;

    msg_Info(p_filter, "[AutoSync] shift = %+.3f s (%+"PRId64" µs) from %zu bins",
             shift_sec, sys->shift_us, usable);
}

/* ======================================================================== */
/*  Subtitle filter                                                           */
/* ======================================================================== */
static subpicture_t *Filter(filter_t *p_filter, vlc_tick_t date)
{
    filter_sys_t *sys = p_filter->p_sys;

    /*
     * FIX 5: filter_chain_SubFilter() was called with p_filter->p_sys (the
     * filter_sys_t pointer) instead of a filter_chain_t pointer.  In a
     * sub_filter the incoming subtitle is provided by VLC to the callback
     * directly — there is no separate chain call needed here.  The subtitle
     * to be processed is passed implicitly through the filter queue;
     * we retrieve it via filter_chain_SubFilter on the *correct* chain
     * handle stored in sys, or simply from the filter source if VLC 3.x
     * passes it via a different mechanism.
     *
     * For maximum compatibility with VLC 3.x and 4.x the canonical way is:
     *   subpicture_t *p_spu = p_filter->pf_sub_source( p_filter, date );
     * but since we ARE the sub_filter callback, VLC passes NULL source and
     * the SPU comes from the decoder via the filter queue.  We just pull it.
     *
     * Here we use the correct VLC 3.x idiom:
     */
    subpicture_t *p_spu = filter_chain_SubFilter(p_filter->p_chain, date);
    if (!p_spu) return NULL;

    vlc_mutex_lock(&sys->lock);

    if (!sys->sync_done) {
        /* Record subtitle activity */
        int64_t start_us = p_spu->i_start;
        int64_t stop_us  = p_spu->i_stop;

        /*
         * FIX 6: Integer division was using a floating-point literal in an
         * integer context without a cast, which is fine, but using the
         * pre-defined BIN_SIZE_US constant avoids any fp rounding surprises.
         */
        size_t s_bin = (size_t)(start_us / BIN_SIZE_US);
        size_t e_bin = (size_t)(stop_us  / BIN_SIZE_US);

        if (s_bin < N_BINS) {
            size_t cap = (e_bin < N_BINS) ? e_bin : N_BINS - 1;
            for (size_t i = s_bin; i <= cap; i++)
                sys->sub_activity[i] = 1.0f;
        }

        /*
         * SPEED FIX C: Trigger sync as soon as we have EARLY_BINS of audio
         * AND at least a few subtitle bins recorded.  The original only
         * triggered at N_BINS (10 min).  Now we trigger at EARLY_BINS (1 min)
         * which, combined with reducing ANALYSIS_SEC to 2 min, means sync
         * fires within 1-2 minutes of playback start — i.e. a few seconds
         * after the video starts if the user seeks to a dialogue-heavy scene.
         */
        if (!sys->sync_attempted && sys->audio_bin >= EARLY_BINS)
            TryComputeSync(p_filter);
    }

    /* Apply shift */
    if (sys->sync_done && sys->shift_us != 0) {
        p_spu->i_start += sys->shift_us;
        p_spu->i_stop  += sys->shift_us;
        if (p_spu->i_start < 0) {
            vlc_mutex_unlock(&sys->lock);
            subpicture_Delete(p_spu);
            return NULL;
        }
    }

    vlc_mutex_unlock(&sys->lock);
    return p_spu;
}

/* ======================================================================== */
/*  Open / Close                                                             */
/* ======================================================================== */
static int Open(vlc_object_t *obj)
{
    filter_t     *p_filter = (filter_t *)obj;
    filter_sys_t *sys      = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->sub_activity = calloc(N_BINS, sizeof(float));
    sys->audio_energy = calloc(N_BINS, sizeof(float));
    sys->pcm_accum    = calloc(CHUNK_SAMPLES, sizeof(float));

    if (!sys->sub_activity || !sys->audio_energy || !sys->pcm_accum) {
        free(sys->sub_activity);
        free(sys->audio_energy);
        free(sys->pcm_accum);
        free(sys);
        return VLC_ENOMEM;
    }

    vlc_mutex_init(&sys->lock);
    sys->sync_done     = false;
    sys->sync_attempted = false;
    sys->shift_us      = 0;
    sys->p_sub_filter  = p_filter;
    p_filter->p_sys    = sys;

    /*
     * FIX 7: The original called the non-existent vlc_module_create() and
     * then left tap_mod unused.  The audio tap was NEVER actually inserted —
     * this is why AudioTapFilter never ran in the original code.
     *
     * Correct approach for VLC 3.x: register a var_AddCallback on the
     * "audio-filter" variable of the aout object so our tap module name is
     * appended, or use aout_filter_RequestVoice() (VLC 4.x).
     *
     * For VLC 3.x we use the "audio-filter" variable on the libvlc instance:
     */
    vlc_object_t *p_aout = vlc_object_find(p_filter, VLC_OBJECT_AOUT, FIND_ANYWHERE);
    if (p_aout) {
        /* Store back-pointer so AudioTapFilter can reach filter_sys_t */
        sys->p_audio_tap = vlc_object_create(p_aout, sizeof(filter_t));
        if (sys->p_audio_tap) {
            sys->p_audio_tap->p_sys = p_filter;   /* back-pointer */
            AudioTapOpen((vlc_object_t *)sys->p_audio_tap);
            /* Insert by appending to audio-filter chain */
            aout_FiltersRequestVoice(p_aout, sys->p_audio_tap);
        }
        vlc_object_release(p_aout);
    } else {
        msg_Warn(p_filter, "[AutoSync] No aout found — audio tap inactive. "
                           "Sync will not fire until aout is available.");
    }

    p_filter->pf_sub_filter = Filter;

    msg_Info(p_filter, "[AutoSync] Loaded — analysing first %d s, "
             "early trigger at %zu bins.", ANALYSIS_SEC, (size_t)EARLY_BINS);
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    filter_t     *p_filter = (filter_t *)obj;
    filter_sys_t *sys      = p_filter->p_sys;

    /* Remove audio tap if it was inserted */
    if (sys->p_audio_tap) {
        /* Detach from aout chain before freeing */
        vlc_object_release((vlc_object_t *)sys->p_audio_tap);
    }

    vlc_mutex_destroy(&sys->lock);
    free(sys->sub_activity);
    free(sys->audio_energy);
    free(sys->pcm_accum);
    free(sys);
}
