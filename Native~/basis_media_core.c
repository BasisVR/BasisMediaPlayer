/*
 * basis_media_core.c — engine lifecycle, demux thread, state machine, and the
 * sink that bridges the portable demuxers to the platform decode backend.
 *
 * One basis_media_engine owns:
 *   - a parsed URL and a platform basis_decoder (OS hardware decode -> GPU texture)
 *   - a demux thread that connects + parses the live stream into elementary
 *     H.264/H.265 + AAC and pushes them through a basis_media_sink
 *   - thread-safe state + last-error string read by the public ABI getters
 *
 * The public ABI getters/setters just delegate to the decoder or read state.
 */

#include "basis_media_native.h"
#include "basis_media_internal.h"

#include "protocol/basis_url.h"
#include "protocol/basis_io.h"
#include "protocol/basis_rtsp.h"
#include "protocol/basis_rtmp.h"
#include "protocol/basis_ts.h"
#include "protocol/basis_mp4.h"
#include "protocol/basis_http.h"
#include "protocol/basis_hls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
  #include "windows/basis_win_http.h"
  typedef HANDLE       basis_thread_t;
  typedef CRITICAL_SECTION basis_mutex_t;
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t        basis_thread_t;
  typedef pthread_mutex_t  basis_mutex_t;
#endif
#if defined(__ANDROID__)
  #include "android/basis_jni_https.h"
  #include <android/log.h>
  #define BASIS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "basis_media", __VA_ARGS__)
#else
  #define BASIS_LOGI(...) ((void)0)
#endif

/* ---- tiny thread/mutex/sleep shims -------------------------------------- */

static void mutex_init(basis_mutex_t* m) {
#if defined(_WIN32)
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}
static void mutex_destroy(basis_mutex_t* m) {
#if defined(_WIN32)
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}
static void mutex_lock(basis_mutex_t* m) {
#if defined(_WIN32)
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}
static void mutex_unlock(basis_mutex_t* m) {
#if defined(_WIN32)
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}
static void sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* ---- engine ------------------------------------------------------------- */

struct basis_media_engine {
    char  url[2048];
    basis_url_t parts;

    basis_decoder_t* decoder;

    basis_thread_t thread;
    int thread_started;
    volatile int running;
    volatile int paused;

    basis_mutex_t lock;
    basis_media_state_t state;
    char error[512];

    basis_media_sink_t sink;

    /* diagnostics (demux thread writes, main thread reads; minor races OK) */
    volatile long video_au_count;
    volatile long audio_frame_count;
};

/* ---- state/error helpers (exported to internal) ------------------------- */

void basis_engine_set_state(basis_media_engine_t* e, basis_media_state_t s) {
    if (!e) return;
    mutex_lock(&e->lock);
    /* don't clobber a terminal error with a later non-error state */
    if (e->state != BASIS_MEDIA_STATE_ERROR || s == BASIS_MEDIA_STATE_ERROR)
        e->state = s;
    mutex_unlock(&e->lock);
}

void basis_engine_set_error(basis_media_engine_t* e, const char* msg) {
    if (!e) return;
    mutex_lock(&e->lock);
    if (msg) {
        strncpy(e->error, msg, sizeof(e->error) - 1);
        e->error[sizeof(e->error) - 1] = 0;
    }
    e->state = BASIS_MEDIA_STATE_ERROR;
    mutex_unlock(&e->lock);
}

basis_decoder_t* basis_engine_get_decoder(basis_media_engine_t* e) { return e ? e->decoder : NULL; }
int basis_engine_is_paused(basis_media_engine_t* e) { return e ? e->paused : 0; }
int basis_engine_is_running(basis_media_engine_t* e) { return e ? e->running : 0; }

/* ---- sink callbacks (run on the demux thread) --------------------------- */

static void sink_video_format(void* user, basis_codec_t codec, const uint8_t* ed, int ed_len, int w, int h) {
    basis_media_engine_t* e = (basis_media_engine_t*)user;
    basis_decoder_set_video_format(e->decoder, codec, ed, ed_len, w, h);
}
static void sink_video_au(void* user, const uint8_t* au, int len, int64_t pts, int key) {
    basis_media_engine_t* e = (basis_media_engine_t*)user;
    if (!e->running) return;
    e->video_au_count++;
    basis_decoder_submit_video(e->decoder, au, len, pts, key);
    /* CONNECTING/BUFFERING -> PLAYING once the OS decoder is actually producing
     * frames (a few buffered), so the state doesn't sit at Buffering forever. */
    if ((e->state == BASIS_MEDIA_STATE_CONNECTING || e->state == BASIS_MEDIA_STATE_BUFFERING) &&
        basis_decoder_get_frame_counter(e->decoder) >= 4)
        basis_engine_set_state(e, BASIS_MEDIA_STATE_PLAYING);
}
static void sink_audio_format(void* user, basis_codec_t codec, int rate, int ch, const uint8_t* asc, int asc_len) {
    basis_media_engine_t* e = (basis_media_engine_t*)user;
    basis_decoder_set_audio_format(e->decoder, codec, rate, ch, asc, asc_len);
}
static void sink_audio_frame(void* user, const uint8_t* data, int len, int64_t pts) {
    basis_media_engine_t* e = (basis_media_engine_t*)user;
    if (!e->running) return;
    e->audio_frame_count++;
    basis_decoder_submit_audio(e->decoder, data, len, pts);
}
static void sink_state(void* user, basis_media_state_t s) { basis_engine_set_state((basis_media_engine_t*)user, s); }
static void sink_error(void* user, const char* m) { basis_engine_set_error((basis_media_engine_t*)user, m); }
static void sink_eos(void* user) { basis_engine_set_state((basis_media_engine_t*)user, BASIS_MEDIA_STATE_ENDED); }
static int  sink_is_running(void* user) { basis_media_engine_t* e = (basis_media_engine_t*)user; return e->running; }

static void install_sink(basis_media_engine_t* e) {
    e->sink.user = e;
    e->sink.on_video_format = sink_video_format;
    e->sink.on_video_au = sink_video_au;
    e->sink.on_audio_format = sink_audio_format;
    e->sink.on_audio_frame = sink_audio_frame;
    e->sink.on_state = sink_state;
    e->sink.on_error = sink_error;
    e->sink.on_end_of_stream = sink_eos;
    e->sink.is_running = sink_is_running;
}

/* ---- demux thread ------------------------------------------------------- */

static int ends_with_ci(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    const char* p = s + (ls - lf);
    for (size_t i = 0; i < lf; ++i) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* HLS / LL-HLS: the URL is a playlist, not a continuous byte stream. The HLS
 * source fetches+parses the M3U8, stitches segments (and LL-HLS parts) into one
 * byte stream, and the existing TS/fMP4 demuxers consume it. Windows fetches via
 * WinHTTP; Android/Quest support is planned. */
static void run_hls(basis_media_engine_t* e) {
#if defined(_WIN32)
    basis_http_provider_t provider = {
        basis_win_http_open, basis_win_http_read, basis_win_http_close
    };
    int is_fmp4 = 0;
    void* hls = basis_hls_open(e->url, &provider, e->sink.is_running, e->sink.user, &is_fmp4);
    if (!hls) {
        basis_engine_set_error(e, "failed to open HLS playlist");
        return;
    }
    basis_engine_set_state(e, BASIS_MEDIA_STATE_BUFFERING);
    if (is_fmp4)
        basis_mp4_run(&e->sink, basis_hls_read, hls);
    else
        basis_ts_run(&e->sink, basis_hls_read, hls);
    basis_hls_close(hls);
#else
    basis_engine_set_error(e, "HLS playback currently requires the Windows backend.");
#endif
}

static void run_http_like(basis_media_engine_t* e) {
    /* Android: the OS extractor can demux the URL itself (TLS included). */
    if (basis_decoder_try_open_url(e->decoder, e->url)) {
        basis_engine_set_state(e, BASIS_MEDIA_STATE_BUFFERING);
        while (e->running) sleep_ms(20);
        return;
    }

    /* HLS playlists are not a single continuous stream — hand off to the HLS
     * source before the plain TS/fMP4 byte-source path. (.m3u8 may carry a query.) */
    if (strstr(e->parts.path, ".m3u8")) {
        run_hls(e);
        return;
    }

    void* src = NULL;
    basis_read_fn rd = NULL;

#if defined(_WIN32)
    src = basis_win_http_open(e->url);   /* WinHTTP: handles http + https/TLS */
    rd = basis_win_http_read;
#elif defined(__ANDROID__)
    /* AMediaExtractor either took the URL (already returned above) or rejected
     * it (unsupported live container, etc). Fall back to a JNI-backed Java
     * HttpsURLConnection feeding the portable TS/MP4 demuxers — same path used
     * for RTSP/RTMP. Works for both http:// and https://.
     *
     * Read timeout is 60s, not 15s: live streams can have brief stalls (key-
     * frame intervals, network jitter, server buffering) that a short timeout
     * would mistake for a dead socket. Connect timeout stays implicitly short
     * (the open call). */
    src = basis_jni_https_open(e->url, 60000);
    rd = basis_jni_https_read;
#else
    if (e->parts.tls) {
        basis_engine_set_error(e, "https requires the platform TLS stack (WinHTTP/AMediaExtractor); not available on this build.");
        return;
    }
    src = basis_http_open(&e->parts, 15000);
    rd = basis_http_read;
#endif

    if (!src) {
        basis_engine_set_error(e, "failed to open HTTP byte source");
        return;
    }

    basis_engine_set_state(e, BASIS_MEDIA_STATE_BUFFERING);
    if (ends_with_ci(e->parts.path, ".mp4") || ends_with_ci(e->parts.path, ".m4s"))
        basis_mp4_run(&e->sink, rd, src);
    else
        basis_ts_run(&e->sink, rd, src); /* default to MPEG-TS (.ts and friends) */

#if defined(_WIN32)
    basis_win_http_close(src);
#elif defined(__ANDROID__)
    basis_jni_https_close(src);
#else
    basis_http_close(src);
#endif
}

/* Dispatch the URL to its protocol handler. Blocks until the stream drops, the
 * engine is stopped, or a hard error is set. */
static void run_protocol_once(basis_media_engine_t* e) {
    if (basis_url_is_rtsp(&e->parts)) {
        basis_rtsp_run(&e->sink, &e->parts);
    } else if (basis_url_is_rtmp(&e->parts)) {
        if (e->parts.tls) {
            basis_engine_set_error(e, "rtmps (RTMP-over-TLS) is not supported; use rtmp:// or an https fMP4/TS URL.");
        } else {
            basis_rtmp_run(&e->sink, &e->parts);
        }
    } else { /* http / https */
        run_http_like(e);
    }
}

/* Sleep that wakes early when the engine is stopped, so teardown never blocks on
 * a reconnect backoff. */
static void sleep_interruptible(basis_media_engine_t* e, int ms) {
    while (ms > 0 && e->running) {
        int chunk = ms < 50 ? ms : 50;
        sleep_ms(chunk);
        ms -= chunk;
    }
}

static int engine_state_is_error(basis_media_engine_t* e) {
    int err;
    mutex_lock(&e->lock);
    err = (e->state == BASIS_MEDIA_STATE_ERROR);
    mutex_unlock(&e->lock);
    return err;
}

/* Demux thread: run the protocol, and on an unexpected drop reconnect with
 * exponential backoff (keeping the decoder + GPU resources alive — far cheaper
 * than a full teardown/reopen). Backoff resets whenever a run actually received
 * media, so brief blips recover instantly while a dead endpoint backs off. A hard
 * error (auth/unsupported) or repeated no-progress failures stop the loop; on
 * give-up we surface ENDED so the upper layer can do a full reopen if it loops. */
static void demux_body(basis_media_engine_t* e) {
    int backoff_ms = 500;
    int attempt = 0;
    const int MAX_ATTEMPTS = 6; /* ~500ms..8s capped: several retries before giving up */

    while (e->running) {
        basis_engine_set_state(e, BASIS_MEDIA_STATE_CONNECTING);
        long au_before = e->video_au_count + e->audio_frame_count;

        run_protocol_once(e);

        if (!e->running) break;              /* user stop */
        if (engine_state_is_error(e)) break; /* hard failure: retrying won't help */

        long au_after = e->video_au_count + e->audio_frame_count;
        long delta = au_after - au_before;
        BASIS_LOGI("demux run ended: delta_aus=%ld total_aus=%ld attempt=%d/%d",
                   delta, au_after, attempt + 1, MAX_ATTEMPTS);
        if (delta > 10) { attempt = 0; backoff_ms = 500; }
        else attempt++;

        if (attempt >= MAX_ATTEMPTS) {
            BASIS_LOGI("demux giving up after %d empty attempts; setting ENDED", MAX_ATTEMPTS);
            basis_engine_set_state(e, BASIS_MEDIA_STATE_ENDED);
            break;
        }

        basis_engine_set_state(e, BASIS_MEDIA_STATE_BUFFERING); /* reconnecting */
        sleep_interruptible(e, backoff_ms);
        backoff_ms *= 2;
        if (backoff_ms > 8000) backoff_ms = 8000;
    }
}

#if defined(_WIN32)
static DWORD WINAPI thread_entry(LPVOID arg) { demux_body((basis_media_engine_t*)arg); return 0; }
#else
static void* thread_entry(void* arg) { demux_body((basis_media_engine_t*)arg); return NULL; }
#endif

static int thread_start(basis_media_engine_t* e) {
#if defined(_WIN32)
    e->thread = CreateThread(NULL, 0, thread_entry, e, 0, NULL);
    return e->thread != NULL;
#else
    return pthread_create(&e->thread, NULL, thread_entry, e) == 0;
#endif
}
static void thread_join(basis_media_engine_t* e) {
    if (!e->thread_started) return;
#if defined(_WIN32)
    WaitForSingleObject(e->thread, INFINITE);
    CloseHandle(e->thread);
#else
    pthread_join(e->thread, NULL);
#endif
    e->thread_started = 0;
}

/* ---- public ABI --------------------------------------------------------- */

BASIS_API basis_media_engine_t* BASIS_CALL basis_media_open(const char* url) {
    if (!url) return NULL;

    basis_media_engine_t* e = (basis_media_engine_t*)calloc(1, sizeof(*e));
    if (!e) return NULL;

    strncpy(e->url, url, sizeof(e->url) - 1);
    if (basis_url_parse(url, &e->parts) != 0) { free(e); return NULL; }

    mutex_init(&e->lock);
    e->state = BASIS_MEDIA_STATE_IDLE;

    basis_io_global_init();

    e->decoder = basis_decoder_create(e);
    if (!e->decoder) {
        basis_io_global_shutdown();
        mutex_destroy(&e->lock);
        free(e);
        return NULL;
    }

    install_sink(e);

    e->running = 1;
    if (!thread_start(e)) {
        e->running = 0;
        basis_decoder_destroy(e->decoder);
        basis_io_global_shutdown();
        mutex_destroy(&e->lock);
        free(e);
        return NULL;
    }
    e->thread_started = 1;
    return e;
}

BASIS_API void BASIS_CALL basis_media_close(basis_media_engine_t* e) {
    if (!e) return;

    /* Stop the demux thread first so nothing submits while we tear down. */
    e->running = 0;
    thread_join(e);

    /* Free OS decode/audio + GPU resources. basis_decoder_destroy calls
     * render_release internally; with the threads joined nothing is mid-decode,
     * and D3D11/D3D12 COM Release is thread-safe. */
    if (e->decoder) {
        basis_decoder_destroy(e->decoder);
        e->decoder = NULL;
    }

    basis_io_global_shutdown();
    mutex_destroy(&e->lock);
    free(e);
}

BASIS_API void BASIS_CALL basis_media_play(basis_media_engine_t* e) {
    if (!e) return;
    e->paused = 0;
    basis_engine_set_state(e, BASIS_MEDIA_STATE_PLAYING);
}

BASIS_API void BASIS_CALL basis_media_pause(basis_media_engine_t* e) {
    if (!e) return;
    e->paused = 1;
    basis_engine_set_state(e, BASIS_MEDIA_STATE_PAUSED);
}

BASIS_API void BASIS_CALL basis_media_stop(basis_media_engine_t* e) {
    if (!e) return;
    e->paused = 1;
    basis_engine_set_state(e, BASIS_MEDIA_STATE_IDLE);
}

BASIS_API int BASIS_CALL basis_media_get_state(basis_media_engine_t* e) {
    if (!e) return BASIS_MEDIA_STATE_IDLE;
    mutex_lock(&e->lock);
    int s = (int)e->state;
    mutex_unlock(&e->lock);
    return s;
}

BASIS_API int BASIS_CALL basis_media_get_video_size(basis_media_engine_t* e, int* w, int* h) {
    if (!e || !e->decoder) return -1;
    return basis_decoder_get_video_size(e->decoder, w, h);
}

BASIS_API int64_t BASIS_CALL basis_media_get_position_us(basis_media_engine_t* e) {
    if (!e || !e->decoder) return -1;
    return basis_decoder_get_position_us(e->decoder);
}

BASIS_API int BASIS_CALL basis_media_get_last_error(basis_media_engine_t* e, char* buf, int buf_size) {
    if (!e || !buf || buf_size <= 0) return 0;
    mutex_lock(&e->lock);
    int n = (int)strlen(e->error);
    if (n >= buf_size) n = buf_size - 1;
    memcpy(buf, e->error, (size_t)n);
    buf[n] = 0;
    mutex_unlock(&e->lock);
    return n;
}

BASIS_API int BASIS_CALL basis_media_get_debug(basis_media_engine_t* e, char* buf, int buf_size) {
    if (!e || !buf || buf_size <= 0) return 0;
    int n = snprintf(buf, (size_t)buf_size, "vau=%ld aau=%ld | ",
                     e->video_au_count, e->audio_frame_count);
    if (n < 0) n = 0;
    if (e->decoder && n < buf_size) n += basis_decoder_get_debug(e->decoder, buf + n, buf_size - n);
    return n;
}

BASIS_API void BASIS_CALL basis_media_set_buffer(basis_media_engine_t* e, int mode, int buffer_ms) {
    if (e && e->decoder) basis_decoder_set_buffer(e->decoder, mode, buffer_ms);
}

BASIS_API void BASIS_CALL basis_media_set_output_texture(basis_media_engine_t* e, void* native_texture, int w, int h) {
    if (e && e->decoder) basis_decoder_set_output_texture(e->decoder, native_texture, w, h);
}

BASIS_API void* BASIS_CALL basis_media_get_texture(basis_media_engine_t* e, int* w, int* h) {
    if (!e || !e->decoder) return NULL;
    return basis_decoder_get_texture(e->decoder, w, h);
}

BASIS_API uint64_t BASIS_CALL basis_media_get_frame_counter(basis_media_engine_t* e) {
    if (!e || !e->decoder) return 0;
    return basis_decoder_get_frame_counter(e->decoder);
}

BASIS_API int BASIS_CALL basis_media_get_audio_format(basis_media_engine_t* e, int* rate, int* ch) {
    if (!e || !e->decoder) return -1;
    return basis_decoder_get_audio_format(e->decoder, rate, ch);
}

BASIS_API int BASIS_CALL basis_media_read_audio(basis_media_engine_t* e, float* out, int max_floats) {
    if (!e || !e->decoder || !out || max_floats <= 0) return 0;
    if (e->paused) return 0; /* silence while paused */
    return basis_decoder_read_audio(e->decoder, out, max_floats);
}

/* The render-event function lives in the platform glue (basis_unity_plugin.cpp);
 * it dispatches BASIS_RENDER_UPDATE/RELEASE to basis_decoder_render_*. */
