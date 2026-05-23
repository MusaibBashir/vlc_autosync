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

#define BIN_SIZE_SEC    0.10f
#define BIN_SIZE_US     100000LL
#define ANALYSIS_SEC    120
#define N_BINS          ((size_t)(ANALYSIS_SEC / BIN_SIZE_SEC))
#define AUDIO_SR        8000
#define CHUNK_SAMPLES   ((int)(AUDIO_SR * BIN_SIZE_SEC))
#define ENERGY_THRESH   1.5f
#define EARLY_BINS      (N_BINS / 2)

/* ── module descriptor ───────────────────────────────────────────────────── */
static int  SubOpen (vlc_object_t *);
static void SubClose(vlc_object_t *);
static int  AudioOpen (vlc_object_t *);
static void AudioClose(vlc_object_t *);

vlc_module_begin()
    set_shortname("AutoSync Sub")
    set_description("Auto Subtitle Sync (Sub Filter)")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_SUBPIC)
    set_capability("sub filter", 0)
    set_callbacks(SubOpen, SubClose)
    add_shortcut("autosync_sub")

    add_submodule()
    set_shortname("AutoSync Audio")
    set_description("Auto Subtitle Sync (Audio Tap)")
    set_category(CAT_AUDIO)
    set_subcategory(SUBCAT_AUDIO_AFILTER)
    set_capability("audio filter", 0)
    set_callbacks(AudioOpen, AudioClose)
    add_shortcut("autosync_audio")
vlc_module_end()

/* ── Shared context ───────────────────────────────────────────────────────── */
/* Decouples the audio tap from the subtitle filter safely for VLC 3.x/4.x */
typedef struct {
    float      *sub_activity;
    float      *audio_energy;
    float      *pcm_accum;
    int         pcm_accum_n;
    size_t      audio_bin;

    vlc_mutex_t lock;
    bool        sync_done;
    bool        sync_attempted;
    int64_t     shift_us;
} autosync_ctx_t;

static struct {
    vlc_mutex_t lock;
    int refcount;
    autosync_ctx_t *ctx;
} g_module = { VLC_STATIC_MUTEX, 0, NULL };

static autosync_ctx_t *GetSharedContext(void) {
    vlc_mutex_lock(&g_module.lock);
    if (!g_module.ctx) {
        g_module.ctx = calloc(1, sizeof(*g_module.ctx));
        g_module.ctx->sub_activity = calloc(N_BINS, sizeof(float));
        g_module.ctx->audio_energy = calloc(N_BINS, sizeof(float));
        g_module.ctx->pcm_accum    = calloc(CHUNK_SAMPLES, sizeof(float));
        vlc_mutex_init(&g_module.ctx->lock);
    }
    g_module.refcount++;
    autosync_ctx_t *res = g_module.ctx;
    vlc_mutex_unlock(&g_module.lock);
    return res;
}

static void ReleaseSharedContext(void) {
    vlc_mutex_lock(&g_module.lock);
    g_module.refcount--;
    if (g_module.refcount == 0 && g_module.ctx) {
        free(g_module.ctx->sub_activity);
        free(g_module.ctx->audio_energy);
        free(g_module.ctx->pcm_accum);
        vlc_mutex_destroy(&g_module.ctx->lock);
        free(g_module.ctx);
        g_module.ctx = NULL;
    }
    vlc_mutex_unlock(&g_module.lock);
}

struct filter_sys_t {
    autosync_ctx_t *ctx;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int cmp_float(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void TryComputeSync(filter_t *p_filter, autosync_ctx_t *ctx) {
    if (ctx->sync_attempted) return;
    ctx->sync_attempted = true;

    size_t usable = ctx->audio_bin;
    if (usable < EARLY_BINS) return;

    float *tmp = malloc(usable * sizeof(float));
    if (!tmp) return;
    memcpy(tmp, ctx->audio_energy, usable * sizeof(float));
    qsort(tmp, usable, sizeof(float), cmp_float);
    float threshold = tmp[usable / 2] * ENERGY_THRESH;
    free(tmp);

    float *audio_bin_arr = calloc(usable, sizeof(float));
    if (!audio_bin_arr) { free(tmp); return; }
    for (size_t i = 0; i < usable; i++)
        audio_bin_arr[i] = (ctx->audio_energy[i] > threshold) ? 1.0f : 0.0f;

    double shift_sec = autosync_compute_shift(audio_bin_arr, ctx->sub_activity, usable, (double)BIN_SIZE_SEC);
    free(audio_bin_arr);

    ctx->shift_us  = (int64_t)(shift_sec * 1000000.0);
    ctx->sync_done = true;

    msg_Info(p_filter, "[AutoSync] shift = %+.3f s (%+"PRId64" us) from %zu bins", shift_sec, ctx->shift_us, usable);
}

/* ── Audio Filter Tap ────────────────────────────────────────────────────── */
static block_t *AudioFilter(filter_t *p_tap, block_t *p_block) {
    filter_sys_t *sys = p_tap->p_sys;
    autosync_ctx_t *ctx = sys->ctx;

    if (!p_block || ctx->sync_done) return p_block;

    int channels = p_tap->fmt_in.audio.i_channels;
    int samples  = (int)p_block->i_nb_samples;
    float *pcm   = (float *)p_block->p_buffer;

    vlc_mutex_lock(&ctx->lock);

    for (int i = 0; i < samples && ctx->audio_bin < N_BINS; i++) {
        float mono = 0.0f;
        for (int c = 0; c < channels; c++) mono += pcm[i * channels + c];
        mono /= (float)channels;

        ctx->pcm_accum[ctx->pcm_accum_n++] = mono;

        if (ctx->pcm_accum_n >= CHUNK_SAMPLES) {
            double sum = 0.0;
            for (int k = 0; k < CHUNK_SAMPLES; k++)
                sum += (double)ctx->pcm_accum[k] * ctx->pcm_accum[k];
            ctx->audio_energy[ctx->audio_bin++] = (float)sqrt(sum / CHUNK_SAMPLES);
            ctx->pcm_accum_n = 0;
        }
    }

    vlc_mutex_unlock(&ctx->lock);
    return p_block;
}

static int AudioOpen(vlc_object_t *obj) {
/* ADD THIS LINE FIRST */
    msg_Err(obj, "[AutoSync DEBUG] ---> AUDIO FILTER ATTEMPTING TO OPEN <---");

    filter_t *p_tap = (filter_t *)obj;
    if (p_tap->fmt_in.audio.i_format != VLC_CODEC_FL32) {
        msg_Err(obj, "[AutoSync DEBUG] Audio format rejected! Expected FL32.");
        return VLC_EGENERIC;
    }

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    
    sys->ctx = GetSharedContext();
    p_tap->p_sys = sys;
    p_tap->pf_audio_filter = AudioFilter;
    
    msg_Err(obj, "[AutoSync DEBUG] ---> AUDIO FILTER SUCCESSFULLY LOADED <---");
    return VLC_SUCCESS;
}

static void AudioClose(vlc_object_t *obj) {
    filter_t *p_tap = (filter_t *)obj;
    ReleaseSharedContext();
    free(p_tap->p_sys);
}

/* ── Subtitle Filter ─────────────────────────────────────────────────────── */
static subpicture_t *SubFilter(filter_t *p_filter, subpicture_t *p_spu) {
    if (!p_spu) return NULL;

    filter_sys_t *sys = p_filter->p_sys;
    autosync_ctx_t *ctx = sys->ctx;

    vlc_mutex_lock(&ctx->lock);

    if (!ctx->sync_done) {
        int64_t start_us = p_spu->i_start;
        int64_t stop_us  = p_spu->i_stop;

        size_t s_bin = (size_t)(start_us / BIN_SIZE_US);
        size_t e_bin = (size_t)(stop_us  / BIN_SIZE_US);

        if (s_bin < N_BINS) {
            size_t cap = (e_bin < N_BINS) ? e_bin : N_BINS - 1;
            for (size_t i = s_bin; i <= cap; i++)
                ctx->sub_activity[i] = 1.0f;
        }

        if (!ctx->sync_attempted && ctx->audio_bin >= EARLY_BINS)
            TryComputeSync(p_filter, ctx);
    }

    if (ctx->sync_done && ctx->shift_us != 0) {
        p_spu->i_start += ctx->shift_us;
        p_spu->i_stop  += ctx->shift_us;
        if (p_spu->i_start < 0) {
            vlc_mutex_unlock(&ctx->lock);
            subpicture_Delete(p_spu);
            return NULL;
        }
    }

    vlc_mutex_unlock(&ctx->lock);
    return p_spu;
}

static int SubOpen(vlc_object_t *obj) {
/* ADD THIS LINE FIRST */
    msg_Err(obj, "[AutoSync DEBUG] ---> SUB FILTER ATTEMPTING TO OPEN <---");

    filter_t *p_filter = (filter_t *)obj;

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    
    sys->ctx = GetSharedContext();
    p_filter->p_sys = sys;
    p_filter->pf_sub_filter = SubFilter;

    msg_Err(obj, "[AutoSync DEBUG] ---> SUB FILTER SUCCESSFULLY LOADED <---");
    return VLC_SUCCESS;
}

static void SubClose(vlc_object_t *obj) {
    filter_t *p_filter = (filter_t *)obj;
    ReleaseSharedContext();
    free(p_filter->p_sys);
}