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
#include <vlc_input.h>
#include <vlc_playlist.h>

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "autosync_fft.h"

/* ── tunables ────────────────────────────────────────────────────────────── */
#define BIN_SIZE_SEC        0.10f
#define BIN_SIZE_US         100000LL
#define AUDIO_SR            8000
#define CHUNK_SAMPLES       ((int)(AUDIO_SR * BIN_SIZE_SEC))   /* 800      */
#define ENERGY_THRESH       1.5f

/* FFmpeg pre-analysis window: skip first 5 min, read 90 s of dialogue */
#define FFMPEG_SEEK_SEC     300
#define FFMPEG_WINDOW_SEC   90
#define FFMPEG_N_BINS       ((size_t)(FFMPEG_WINDOW_SEC / BIN_SIZE_SEC))  /* 900 */

/* Live-audio fallback window (used only if FFmpeg fails) */
#define FALLBACK_ANALYSIS_SEC  120
#define FALLBACK_N_BINS        ((size_t)(FALLBACK_ANALYSIS_SEC / BIN_SIZE_SEC)) /* 1200 */
#define FALLBACK_EARLY_BINS    (FALLBACK_N_BINS / 2)

/* Largest array we ever allocate — the fallback window is bigger */
#define N_BINS              FALLBACK_N_BINS

/* ── module descriptor ───────────────────────────────────────────────────── */
static int  SubOpen  (vlc_object_t *);
static void SubClose (vlc_object_t *);
static int  AudioOpen(vlc_object_t *);
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

/* ── Shared context ──────────────────────────────────────────────────────── */
typedef struct {
    /* subtitle activity — recorded by SubFilter from VLC's decoded SPUs */
    float      *sub_activity;       /* [N_BINS]        */

    /* live audio fallback — filled by AudioFilter if FFmpeg pre-analysis fails */
    float      *audio_energy;       /* [N_BINS]        */
    float      *pcm_accum;          /* [CHUNK_SAMPLES] */
    int         pcm_accum_n;
    size_t      audio_bin;

    /* sync result */
    vlc_mutex_t lock;
    bool        sync_done;          /* shift has been computed and applied  */
    bool        ffmpeg_launched;    /* pre-analysis thread already spawned  */
    bool        fallback_attempted; /* live-audio fallback already tried    */
    int64_t     shift_us;

    /* video path — filled by SubOpen, read by the pre-analysis thread */
    char        video_path[4096];
} autosync_ctx_t;

/* Global singleton — both sub filter and audio tap share one context */
static struct {
    vlc_mutex_t    lock;
    int            refcount;
    autosync_ctx_t *ctx;
} g_module = { VLC_STATIC_MUTEX, 0, NULL };

static autosync_ctx_t *GetSharedContext(void)
{
    vlc_mutex_lock(&g_module.lock);
    if (!g_module.ctx) {
        autosync_ctx_t *c = calloc(1, sizeof(*c));
        c->sub_activity = calloc(N_BINS, sizeof(float));
        c->audio_energy = calloc(N_BINS, sizeof(float));
        c->pcm_accum    = calloc(CHUNK_SAMPLES, sizeof(float));
        vlc_mutex_init(&c->lock);
        g_module.ctx = c;
    }
    g_module.refcount++;
    autosync_ctx_t *res = g_module.ctx;
    vlc_mutex_unlock(&g_module.lock);
    return res;
}

static void ReleaseSharedContext(void)
{
    vlc_mutex_lock(&g_module.lock);
    if (--g_module.refcount == 0 && g_module.ctx) {
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
    filter_t       *p_filter;   /* back-pointer for msg_* in thread */
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

/* Binarise an energy array in-place: values above median*thresh become 1 */
static float *BinariseEnergy(const float *energy, size_t n)
{
    float *tmp = malloc(n * sizeof(float));
    float *out = calloc(n, sizeof(float));
    if (!tmp || !out) { free(tmp); free(out); return NULL; }

    memcpy(tmp, energy, n * sizeof(float));
    qsort(tmp, n, sizeof(float), cmp_float);
    float threshold = tmp[n / 2] * ENERGY_THRESH;
    free(tmp);

    for (size_t i = 0; i < n; i++)
        out[i] = (energy[i] > threshold) ? 1.0f : 0.0f;
    return out;
}

/* ── FFmpeg pre-analysis thread (Pathway 3) ──────────────────────────────── */
/*
 * Runs entirely in the background. Calls ffmpeg to extract a 90-second PCM
 * window starting at the 5-minute mark, computes the FFT correlation against
 * the subtitle activity array, and writes the result into ctx->shift_us.
 *
 * If ffmpeg is not on PATH or the file is unreadable, it exits cleanly and
 * the live audio tap (AudioFilter) continues as a fallback.
 */
typedef struct {
    autosync_ctx_t *ctx;
    filter_t       *p_filter;
} preanalysis_args_t;

static void *PreAnalysisThread(void *opaque)
{
    preanalysis_args_t *args      = opaque;
    autosync_ctx_t     *ctx       = args->ctx;
    filter_t           *p_filter  = args->p_filter;
    free(args);

    /* ── 1. Build ffmpeg command ── */
    /*
     * -ss SEEK: seek before opening input (fast, keyframe-accurate enough).
     * -t  WINDOW: read exactly FFMPEG_WINDOW_SEC seconds.
     * Output: mono, 8 kHz, raw float32 little-endian on stdout.
     * -v quiet: suppress ffmpeg banner so we only get PCM bytes.
     */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -ss %d -t %d -i \"%s\" "
        "-ac 1 -ar %d -f f32le -v quiet pipe:1",
        FFMPEG_SEEK_SEC, FFMPEG_WINDOW_SEC,
        ctx->video_path, AUDIO_SR);

    msg_Info(p_filter, "[AutoSync] Pre-analysis: %s", cmd);

    FILE *fp = popen(cmd, "rb");
    if (!fp) {
        msg_Warn(p_filter, "[AutoSync] popen(ffmpeg) failed — live tap will handle sync");
        return NULL;
    }

    /* ── 2. Read PCM ── */
    size_t max_samples = (size_t)(AUDIO_SR) * FFMPEG_WINDOW_SEC;
    float *pcm = malloc(max_samples * sizeof(float));
    if (!pcm) { pclose(fp); return NULL; }

    size_t n_read = fread(pcm, sizeof(float), max_samples, fp);
    pclose(fp);

    if (n_read < (size_t)(AUDIO_SR * 10)) {
        /* Less than 10 s of audio — file unreadable or seek past EOF */
        msg_Warn(p_filter,
            "[AutoSync] FFmpeg returned only %zu samples — falling back to live tap",
            n_read);
        free(pcm);
        return NULL;
    }

    /* ── 3. Compute RMS energy bins ── */
    size_t n_bins = n_read / CHUNK_SAMPLES;
    if (n_bins > FFMPEG_N_BINS) n_bins = FFMPEG_N_BINS;

    float *energy = calloc(n_bins, sizeof(float));
    if (!energy) { free(pcm); return NULL; }

    for (size_t b = 0; b < n_bins; b++) {
        double sum = 0.0;
        const float *chunk = pcm + b * CHUNK_SAMPLES;
        for (int k = 0; k < CHUNK_SAMPLES; k++)
            sum += (double)chunk[k] * chunk[k];
        energy[b] = (float)sqrt(sum / CHUNK_SAMPLES);
    }
    free(pcm);

    /* ── 4. Binarise audio energy ── */
    float *audio_bin = BinariseEnergy(energy, n_bins);
    free(energy);
    if (!audio_bin) return NULL;

    /* ── 5. Extract matching subtitle window ──
     *
     * sub_activity[] is indexed from t=0 in 0.1 s bins.
     * The FFmpeg window covers [FFMPEG_SEEK_SEC, FFMPEG_SEEK_SEC + FFMPEG_WINDOW_SEC].
     * We pull out exactly that slice so the two arrays align before the FFT.
     */
    size_t seek_bin = (size_t)(FFMPEG_SEEK_SEC / BIN_SIZE_SEC);

    float *sub_win = calloc(n_bins, sizeof(float));
    if (!sub_win) { free(audio_bin); return NULL; }

    vlc_mutex_lock(&ctx->lock);
    for (size_t i = 0; i < n_bins; i++) {
        size_t src = seek_bin + i;
        if (src < N_BINS)
            sub_win[i] = ctx->sub_activity[src];
    }
    vlc_mutex_unlock(&ctx->lock);

    /* ── 6. FFT cross-correlation ── */
    double shift_sec = autosync_compute_shift(
        audio_bin, sub_win, n_bins, (double)BIN_SIZE_SEC);
    free(audio_bin);
    free(sub_win);

    /* ── 7. Commit result (only if sync not already done by fallback) ── */
    vlc_mutex_lock(&ctx->lock);
    if (!ctx->sync_done) {
        ctx->shift_us  = (int64_t)(shift_sec * 1000000.0);
        ctx->sync_done = true;
        msg_Info(p_filter,
            "[AutoSync] FFmpeg pre-analysis done: shift = %+.3f s (%+"PRId64" us)",
            shift_sec, ctx->shift_us);
    }
    vlc_mutex_unlock(&ctx->lock);

    return NULL;
}

/* ── Live-audio fallback (used only when FFmpeg pre-analysis fails) ──────── */
static void TryFallbackSync(filter_t *p_filter, autosync_ctx_t *ctx)
{
    if (ctx->fallback_attempted) return;
    ctx->fallback_attempted = true;

    size_t usable = ctx->audio_bin;
    if (usable < FALLBACK_EARLY_BINS) return;

    float *audio_bin = BinariseEnergy(ctx->audio_energy, usable);
    if (!audio_bin) return;

    double shift_sec = autosync_compute_shift(
        audio_bin, ctx->sub_activity, usable, (double)BIN_SIZE_SEC);
    free(audio_bin);

    ctx->shift_us  = (int64_t)(shift_sec * 1000000.0);
    ctx->sync_done = true;

    msg_Info(p_filter,
        "[AutoSync] Fallback live-audio sync: shift = %+.3f s (%+"PRId64" us)",
        shift_sec, ctx->shift_us);
}

/* ── Audio filter tap (fallback only) ───────────────────────────────────── */
static block_t *AudioFilter(filter_t *p_tap, block_t *p_block)
{
    filter_sys_t   *sys = p_tap->p_sys;
    autosync_ctx_t *ctx = sys->ctx;

    /* Once FFmpeg pre-analysis writes sync_done, we stop collecting */
    if (!p_block || ctx->sync_done) return p_block;

    int    channels = p_tap->fmt_in.audio.i_channels;
    int    samples  = (int)p_block->i_nb_samples;
    float *pcm      = (float *)p_block->p_buffer;

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

    /* Trigger fallback if we have enough live audio and FFmpeg hasn't finished */
    if (!ctx->sync_done && ctx->audio_bin >= FALLBACK_EARLY_BINS)
        TryFallbackSync(p_tap, ctx);

    vlc_mutex_unlock(&ctx->lock);
    return p_block;
}

static int AudioOpen(vlc_object_t *obj)
{
    filter_t *p_tap = (filter_t *)obj;

    if (p_tap->fmt_in.audio.i_format != VLC_CODEC_FL32) {
        msg_Dbg(obj, "[AutoSync] Audio format is not FL32 — tap inactive");
        return VLC_EGENERIC;
    }

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->ctx      = GetSharedContext();
    sys->p_filter = p_tap;
    p_tap->p_sys  = sys;
    p_tap->pf_audio_filter = AudioFilter;

    msg_Dbg(obj, "[AutoSync] Audio tap loaded (fallback mode)");
    return VLC_SUCCESS;
}

static void AudioClose(vlc_object_t *obj)
{
    filter_t *p_tap = (filter_t *)obj;
    ReleaseSharedContext();
    free(p_tap->p_sys);
}

/* ── Subtitle filter ─────────────────────────────────────────────────────── */
static subpicture_t *SubFilter(filter_t *p_filter, subpicture_t *p_spu)
{
    if (!p_spu) return NULL;

    filter_sys_t   *sys = p_filter->p_sys;
    autosync_ctx_t *ctx = sys->ctx;

    vlc_mutex_lock(&ctx->lock);

    /* Record subtitle activity into the full N_BINS array (t=0 relative) */
    if (!ctx->sync_done) {
        size_t s_bin = (size_t)(p_spu->i_start / BIN_SIZE_US);
        size_t e_bin = (size_t)(p_spu->i_stop  / BIN_SIZE_US);

        if (s_bin < N_BINS) {
            size_t cap = (e_bin < N_BINS) ? e_bin : N_BINS - 1;
            for (size_t i = s_bin; i <= cap; i++)
                ctx->sub_activity[i] = 1.0f;
        }
    }

    /* Apply shift once available */
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

/* ── SubOpen: extract video path and launch pre-analysis thread ──────────── */
static int SubOpen(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->ctx      = GetSharedContext();
    sys->p_filter = p_filter;
    p_filter->p_sys        = sys;
    p_filter->pf_sub_filter = SubFilter;

    autosync_ctx_t *ctx = sys->ctx;

    /* ── Get video file path from the current input item ── */
    vlc_mutex_lock(&ctx->lock);
    bool need_thread = !ctx->ffmpeg_launched;
    if (need_thread) ctx->ffmpeg_launched = true;
    vlc_mutex_unlock(&ctx->lock);

    if (!need_thread) {
        /* Audio submodule opened first — thread already running */
        msg_Dbg(obj, "[AutoSync] Sub filter loaded, pre-analysis already running");
        return VLC_SUCCESS;
    }

    /*
     * Retrieve the URI of the currently playing item.
     * p_filter sits inside the input/decoder chain, so we walk up to find
     * the playlist and ask for the current item.
     */
    char video_path[4096] = {0};

    input_thread_t *p_input =
        (input_thread_t *)vlc_object_find(p_filter, VLC_OBJECT_INPUT, FIND_ANYWHERE);

    if (p_input) {
        input_item_t *p_item = input_GetItem(p_input);
        if (p_item) {
            char *uri = input_item_GetURI(p_item);
            if (uri) {
                /*
                 * Strip the file:// prefix so ffmpeg gets a plain path.
                 * VLC on Windows uses file:///C:/... — strip three slashes.
                 * On Linux/macOS: file:///home/... — also three slashes.
                 */
                const char *path = uri;
                if (strncmp(path, "file:///", 8) == 0) {
#ifdef _WIN32
                    path += 8;   /* "C:/path/to/movie.mkv" */
#else
                    path += 7;   /* "/home/user/movie.mkv" */
#endif
                }
                snprintf(video_path, sizeof(video_path), "%s", path);

                /* URL-decode spaces (%20) and common special chars */
                /* Simple in-place decode for the most common case */
                char *src = video_path, *dst = video_path;
                while (*src) {
                    if (*src == '%' && src[1] && src[2]) {
                        char hex[3] = { src[1], src[2], 0 };
                        *dst++ = (char)strtol(hex, NULL, 16);
                        src += 3;
                    } else {
                        *dst++ = *src++;
                    }
                }
                *dst = '\0';

                free(uri);
            }
        }
        vlc_object_release(p_input);
    }

    if (video_path[0] == '\0') {
        msg_Warn(obj, "[AutoSync] Could not determine video path — live tap only");
        return VLC_SUCCESS;
    }

    vlc_mutex_lock(&ctx->lock);
    snprintf(ctx->video_path, sizeof(ctx->video_path), "%s", video_path);
    vlc_mutex_unlock(&ctx->lock);

    msg_Info(obj, "[AutoSync] Launching FFmpeg pre-analysis on: %s", video_path);

    /* ── Spawn pre-analysis thread (fire and forget) ── */
    preanalysis_args_t *pargs = calloc(1, sizeof(*pargs));
    if (!pargs) return VLC_SUCCESS;   /* non-fatal — live tap will handle it */
    pargs->ctx      = ctx;
    pargs->p_filter = p_filter;

    vlc_thread_t thread;
    if (vlc_clone(&thread, PreAnalysisThread, pargs, VLC_THREAD_PRIORITY_LOW) != 0) {
        msg_Warn(obj, "[AutoSync] Could not spawn pre-analysis thread — live tap only");
        free(pargs);
        return VLC_SUCCESS;
    }
    vlc_detach(thread);   /* fire and forget — thread frees pargs itself */

    msg_Dbg(obj, "[AutoSync] Sub filter loaded");
    return VLC_SUCCESS;
}

static void SubClose(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    ReleaseSharedContext();
    free(p_filter->p_sys);
}