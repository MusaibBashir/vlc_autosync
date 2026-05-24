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
#include <stdio.h>

#include "autosync_fft.h"

/* ── tunables ────────────────────────────────────────────────────────────── */
#define BIN_SIZE_SEC        0.10f
#define BIN_SIZE_US         100000LL
#define AUDIO_SR            8000
#define CHUNK_SAMPLES       ((int)(AUDIO_SR * BIN_SIZE_SEC))  /* 800       */
#define ENERGY_THRESH       1.5f

/* FFmpeg pre-analysis: skip first 5 min, analyse 90 s of dialogue */
#define FFMPEG_SEEK_SEC     300
#define FFMPEG_WINDOW_SEC   90
#define FFMPEG_N_BINS       ((size_t)(FFMPEG_WINDOW_SEC / BIN_SIZE_SEC))  /* 900 */

/* Live-audio fallback window */
#define FALLBACK_SEC        120
#define FALLBACK_N_BINS     ((size_t)(FALLBACK_SEC / BIN_SIZE_SEC))       /* 1200 */
#define FALLBACK_EARLY      (FALLBACK_N_BINS / 2)

/* Largest array we ever need */
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
    float      *sub_activity;       /* [N_BINS] subtitle presence            */
    float      *audio_energy;       /* [N_BINS] live RMS energy (fallback)   */
    float      *pcm_accum;          /* [CHUNK_SAMPLES]                       */
    int         pcm_accum_n;
    size_t      audio_bin;

    vlc_mutex_t lock;
    bool        sync_done;
    bool        ffmpeg_launched;
    bool        fallback_attempted;
    int64_t     shift_us;

    char        video_path[4096];

    /*
     * Thread handle stored so SubClose() can join it.
     * Fire-and-forget (vlc_detach) is NOT in VLC 3.x public headers.
     * We store the handle and join with zero timeout in Close instead.
     */
    vlc_thread_t preanalysis_thread;
    bool         thread_started;
} autosync_ctx_t;

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
        if (c) {
            c->sub_activity = calloc(N_BINS, sizeof(float));
            c->audio_energy = calloc(N_BINS, sizeof(float));
            c->pcm_accum    = calloc(CHUNK_SAMPLES, sizeof(float));
            vlc_mutex_init(&c->lock);
        }
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
        /* Join the pre-analysis thread if it was started */
        if (g_module.ctx->thread_started)
            vlc_join(g_module.ctx->preanalysis_thread, NULL);
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
    filter_t       *p_filter;
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static float *BinariseEnergy(const float *energy, size_t n)
{
    float *tmp = malloc(n * sizeof(float));
    float *out = calloc(n, sizeof(float));
    if (!tmp || !out) { free(tmp); free(out); return NULL; }
    memcpy(tmp, energy, n * sizeof(float));
    qsort(tmp, n, sizeof(float), cmp_float);
    float thr = tmp[n / 2] * ENERGY_THRESH;
    free(tmp);
    for (size_t i = 0; i < n; i++)
        out[i] = (energy[i] > thr) ? 1.0f : 0.0f;
    return out;
}

/* ── URL-decode a file:// URI into a plain filesystem path ──────────────── */
/*
 * VLC 3.x gives us file:///C:/path/movie.mkv on Windows
 *                   and file:///home/user/movie.mkv on Linux.
 * We strip the scheme and percent-decode the result.
 */
static void UriToPath(const char *uri, char *out, size_t out_sz)
{
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) {
        p += 7;
#ifdef _WIN32
        /* file:///C:/... → skip the leading slash before the drive letter */
        if (p[0] == '/' && p[1] != '\0' && p[2] == ':')
            p++;
#endif
    }

    char *dst = out;
    char *end = out + out_sz - 1;
    while (*p && dst < end) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';

#ifdef _WIN32
    /* Convert forward slashes to backslashes for Windows */
    for (char *c = out; *c; c++)
        if (*c == '/') *c = '\\';
#endif
}

/* ── FFmpeg pre-analysis thread (Pathway 3) ──────────────────────────────── */
typedef struct {
    autosync_ctx_t *ctx;
    filter_t       *p_filter;
} preanalysis_args_t;

static void *PreAnalysisThread(void *opaque)
{
    preanalysis_args_t *args     = opaque;
    autosync_ctx_t     *ctx      = args->ctx;
    filter_t           *p_filter = args->p_filter;
    free(args);

    /* ── 1. Build ffmpeg command ── */
    /*
     * Seeks to FFMPEG_SEEK_SEC before opening the input (fast keyframe seek).
     * Reads FFMPEG_WINDOW_SEC seconds of audio.
     * Outputs: mono, 8 kHz, raw float32 LE on stdout via pipe:1.
     * -v quiet suppresses the banner so only raw PCM bytes come through.
     *
     * On Windows, popen() uses cmd.exe so the command must use double quotes.
     * The video_path may contain spaces — it is already a plain FS path.
     */
    char cmd[8192];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -ss %d -t %d -i \"%s\" "
        "-ac 1 -ar %d -f f32le -v quiet pipe:1",
        FFMPEG_SEEK_SEC, FFMPEG_WINDOW_SEC, ctx->video_path, AUDIO_SR);
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -ss %d -t %d -i '%s' "
        "-ac 1 -ar %d -f f32le -v quiet pipe:1",
        FFMPEG_SEEK_SEC, FFMPEG_WINDOW_SEC, ctx->video_path, AUDIO_SR);
#endif

    msg_Info(p_filter, "[AutoSync] Pre-analysis command: %s", cmd);

    FILE *fp = popen(cmd, "rb");
    if (!fp) {
        msg_Warn(p_filter, "[AutoSync] popen(ffmpeg) failed — live tap fallback active");
        return NULL;
    }

    /* ── 2. Read raw PCM ── */
    size_t max_samples = (size_t)AUDIO_SR * FFMPEG_WINDOW_SEC;
    float *pcm = malloc(max_samples * sizeof(float));
    if (!pcm) { pclose(fp); return NULL; }

    size_t n_read = fread(pcm, sizeof(float), max_samples, fp);
    int exit_code = pclose(fp);

    if (n_read < (size_t)(AUDIO_SR * 5)) {
        msg_Warn(p_filter,
            "[AutoSync] FFmpeg returned only %zu samples (exit %d) — "
            "file may be too short or ffmpeg not on PATH. Live tap fallback active.",
            n_read, exit_code);
        free(pcm);
        return NULL;
    }

    /* ── 3. Compute RMS energy bins ── */
    size_t n_bins = n_read / (size_t)CHUNK_SAMPLES;
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

    /* ── 4. Binarise ── */
    float *audio_bin = BinariseEnergy(energy, n_bins);
    free(energy);
    if (!audio_bin) return NULL;

    /* ── 5. Extract aligned subtitle window ──
     *
     * sub_activity[] is indexed from t=0 in 0.1 s bins.
     * FFmpeg window covers [FFMPEG_SEEK_SEC, FFMPEG_SEEK_SEC + FFMPEG_WINDOW_SEC].
     * Pull that slice so both arrays share the same time origin before the FFT.
     */
    size_t seek_bin = (size_t)((double)FFMPEG_SEEK_SEC / BIN_SIZE_SEC);
    float *sub_win  = calloc(n_bins, sizeof(float));
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

    /* ── 7. Commit (only if the live fallback hasn't already written a result) ── */
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

/* ── Live-audio fallback ─────────────────────────────────────────────────── */
static void TryFallbackSync(filter_t *p_filter, autosync_ctx_t *ctx)
{
    if (ctx->fallback_attempted) return;
    ctx->fallback_attempted = true;

    size_t usable = ctx->audio_bin;
    if (usable < FALLBACK_EARLY) return;

    float *audio_bin = BinariseEnergy(ctx->audio_energy, usable);
    if (!audio_bin) return;

    double shift_sec = autosync_compute_shift(
        audio_bin, ctx->sub_activity, usable, (double)BIN_SIZE_SEC);
    free(audio_bin);

    ctx->shift_us  = (int64_t)(shift_sec * 1000000.0);
    ctx->sync_done = true;
    msg_Info(p_filter,
        "[AutoSync] Live-tap fallback sync done: shift = %+.3f s (%+"PRId64" us)",
        shift_sec, ctx->shift_us);
}

/* ── Audio filter tap ────────────────────────────────────────────────────── */
static block_t *AudioFilter(filter_t *p_tap, block_t *p_block)
{
    filter_sys_t   *sys = p_tap->p_sys;
    autosync_ctx_t *ctx = sys->ctx;

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
            ctx->audio_energy[ctx->audio_bin++] =
                (float)sqrt(sum / CHUNK_SAMPLES);
            ctx->pcm_accum_n = 0;
        }
    }

    if (!ctx->sync_done && ctx->audio_bin >= FALLBACK_EARLY)
        TryFallbackSync(p_tap, ctx);

    vlc_mutex_unlock(&ctx->lock);
    return p_block;
}

static int AudioOpen(vlc_object_t *obj)
{
    filter_t *p_tap = (filter_t *)obj;

    /* We only handle float32 PCM — VLC's aout chain resamples to this */
    if (p_tap->fmt_in.audio.i_format != VLC_CODEC_FL32) {
        msg_Dbg(obj, "[AutoSync] Audio tap: format not FL32, skipping");
        return VLC_EGENERIC;
    }

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->ctx      = GetSharedContext();
    sys->p_filter = p_tap;
    p_tap->p_sys  = sys;
    p_tap->pf_audio_filter = AudioFilter;

    msg_Dbg(obj, "[AutoSync] Audio tap loaded (live fallback ready)");
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

    /* Always record subtitle activity — both FFmpeg thread and live tap use it */
    if (!ctx->sync_done) {
        size_t s_bin = (size_t)((uint64_t)p_spu->i_start / BIN_SIZE_US);
        size_t e_bin = (size_t)((uint64_t)p_spu->i_stop  / BIN_SIZE_US);
        if (s_bin < N_BINS) {
            size_t cap = (e_bin < N_BINS) ? e_bin : N_BINS - 1;
            for (size_t i = s_bin; i <= cap; i++)
                ctx->sub_activity[i] = 1.0f;
        }
    }

    /* Apply shift once computed */
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

/* ── SubOpen: get video path, launch pre-analysis thread ─────────────────── */
/*
 * Getting the video path in a VLC 3.x sub_filter:
 *
 * vlc_object_find() with VLC_OBJECT_INPUT / FIND_ANYWHERE was REMOVED in
 * VLC 3.x (it existed in VLC 2.x only). The correct VLC 3.x approach is:
 *
 *   var_InheritString(p_filter, "input-current")
 *
 * Every input_thread_t exports "input-current" as the MRL of the currently
 * playing item. VLC's variable inheritance walks up the object tree
 * automatically, so calling var_InheritString on ANY object in the chain
 * (including our filter) will find it on the input_thread_t above us.
 *
 * This is the approach used by actual VLC 3.x subtitle and demux plugins.
 */
static int SubOpen(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;

    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->ctx      = GetSharedContext();
    sys->p_filter = p_filter;
    p_filter->p_sys         = sys;
    p_filter->pf_sub_filter = SubFilter;

    autosync_ctx_t *ctx = sys->ctx;

    /* Only the first opener (sub or audio) launches the thread */
    vlc_mutex_lock(&ctx->lock);
    bool need_thread = !ctx->ffmpeg_launched;
    if (need_thread) ctx->ffmpeg_launched = true;
    vlc_mutex_unlock(&ctx->lock);

    if (!need_thread) {
        msg_Dbg(obj, "[AutoSync] Sub filter loaded (thread already running)");
        return VLC_SUCCESS;
    }

    /* ── Retrieve the video MRL via variable inheritance (VLC 3.x API) ── */
    char *mrl = var_InheritString(p_filter, "input-current");
    if (!mrl) {
        /*
         * "input-current" isn't set yet if the filter loads before the
         * input thread fully initialises. Try the parent object explicitly.
         */
        mrl = var_GetNonEmptyString(p_filter->obj.parent, "input-current");
    }

    if (!mrl || mrl[0] == '\0') {
        msg_Warn(obj, "[AutoSync] Could not read video path — live tap only");
        free(mrl);
        return VLC_SUCCESS;
    }

    /* Decode the URI into a filesystem path */
    char video_path[4096];
    UriToPath(mrl, video_path, sizeof(video_path));
    free(mrl);

    if (video_path[0] == '\0') {
        msg_Warn(obj, "[AutoSync] URI decode failed — live tap only");
        return VLC_SUCCESS;
    }

    vlc_mutex_lock(&ctx->lock);
    snprintf(ctx->video_path, sizeof(ctx->video_path), "%s", video_path);
    vlc_mutex_unlock(&ctx->lock);

    msg_Info(obj, "[AutoSync] Launching FFmpeg pre-analysis: %s", video_path);

    /* ── Spawn the pre-analysis thread ──
     *
     * vlc_detach() does NOT exist in VLC 3.x public headers.
     * Instead we store the thread handle in ctx and join it in
     * ReleaseSharedContext() (called from SubClose / AudioClose).
     * The join is non-blocking in practice because the thread finishes
     * within seconds and Close() is only called when the user stops the video.
     */
    preanalysis_args_t *pargs = calloc(1, sizeof(*pargs));
    if (!pargs) return VLC_SUCCESS;   /* non-fatal */
    pargs->ctx      = ctx;
    pargs->p_filter = p_filter;

    if (vlc_clone(&ctx->preanalysis_thread, PreAnalysisThread,
                  pargs, VLC_THREAD_PRIORITY_LOW) != 0) {
        msg_Warn(obj, "[AutoSync] vlc_clone failed — live tap only");
        free(pargs);
        return VLC_SUCCESS;
    }

    ctx->thread_started = true;
    msg_Dbg(obj, "[AutoSync] Sub filter loaded, pre-analysis thread running");
    return VLC_SUCCESS;
}

static void SubClose(vlc_object_t *obj)
{
    filter_t *p_filter = (filter_t *)obj;
    /* ReleaseSharedContext joins the thread when refcount hits 0 */
    ReleaseSharedContext();
    free(p_filter->p_sys);
}