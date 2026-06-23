/*
 * basis_android_decode.c — Android/Quest OS-codec backend (implements basis_decoder_*).
 *
 * Two entry paths:
 *   try_open_url  — https TS/MP4: AMediaExtractor (TLS+demux) -> AMediaCodec.
 *                   A worker thread pumps samples and renders to a Surface.
 *   submit_video  — rtsp/rtmp: the core demuxer feeds Annex-B AUs; we create the
 *                   codec from the SPS/PPS extradata and queue AUs directly.
 *
 * Video output goes to an AImageReader Surface in HardwareBuffer mode; each
 * decoded AHardwareBuffer is handed to the Vulkan present (basis_android_vk),
 * which imports it and resolves YCbCr -> an RGBA VkImage Unity samples.
 * Audio (AAC) is decoded to PCM and written to a ring the C# sink pulls.
 */

#include "../basis_media_internal.h"
#include "basis_android_vk.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <media/NdkImage.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <android/log.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- PCM ring ----------------------------------------------------------- */

typedef struct {
    float* buf; int cap, head, tail;
    pthread_mutex_t m;
} pcm_ring;

static void ring_init(pcm_ring* r, int floats) { r->buf = malloc(sizeof(float) * floats); r->cap = floats; r->head = r->tail = 0; pthread_mutex_init(&r->m, NULL); }
static void ring_free(pcm_ring* r) { free(r->buf); r->buf = NULL; pthread_mutex_destroy(&r->m); }
static void ring_write(pcm_ring* r, const float* s, int n) {
    pthread_mutex_lock(&r->m);
    for (int i = 0; i < n; ++i) { int nt = (r->tail + 1) % r->cap; if (nt == r->head) r->head = (r->head + 1) % r->cap; r->buf[r->tail] = s[i]; r->tail = nt; }
    pthread_mutex_unlock(&r->m);
}
static int ring_read(pcm_ring* r, float* out, int n) {
    pthread_mutex_lock(&r->m);
    int got = 0; while (got < n && r->head != r->tail) { out[got++] = r->buf[r->head]; r->head = (r->head + 1) % r->cap; }
    pthread_mutex_unlock(&r->m);
    return got;
}

/* ---- decoder ------------------------------------------------------------ */

struct basis_decoder {
    basis_media_engine_t* engine;

    AImageReader* reader;
    ANativeWindow* window;

    AMediaCodec* vcodec;
    AMediaCodec* acodec;
    AMediaExtractor* extractor;
    int video_track, audio_track;

    basis_codec_t vc;
    int vw, vh;
    int vconfigured, aconfigured;

    int asr, ach;

    basis_vk_present* vk;

    pthread_t worker;
    int worker_started;

    int64_t lastPtsUs;
    pcm_ring pcm;
};

/* ---- AImageReader callback: capture the decoded AHardwareBuffer ---------- */

static void on_image(void* ctx, AImageReader* reader) {
    basis_decoder_t* d = (basis_decoder_t*)ctx;
    AImage* img = NULL;
    if (AImageReader_acquireLatestImage(reader, &img) != AMEDIA_OK || !img) return;

    AHardwareBuffer* ahb = NULL;
    if (AImage_getHardwareBuffer(img, &ahb) == AMEDIA_OK && ahb) {
        int w = d->vw, h = d->vh;
        int32_t aw = 0, ah = 0;
        AImage_getWidth(img, &aw); AImage_getHeight(img, &ah);
        if (aw > 0) w = aw; if (ah > 0) h = ah;
        if (d->vk) basis_vk_set_hardware_buffer(d->vk, ahb, w, h); /* present acquires its own ref */
    }
    AImage_delete(img);
}

static int ensure_reader(basis_decoder_t* d, int w, int h) {
    if (d->reader) return 0;
    media_status_t st = AImageReader_newWithUsage(
        w, h, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &d->reader);
    if (st != AMEDIA_OK || !d->reader) { basis_engine_set_error(d->engine, "AImageReader_newWithUsage failed"); return -1; }

    AImageReader_ImageListener listener = { d, on_image };
    AImageReader_setImageListener(d->reader, &listener);
    AImageReader_getWindow(d->reader, &d->window);
    d->vw = w; d->vh = h;
    return 0;
}

/* ---- output draining (push decoded frames to the Surface) --------------- */

static void drain_video_output(basis_decoder_t* d) {
    if (!d->vcodec) return;
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(d->vcodec, &info, 0);
        if (oi >= 0) {
            d->lastPtsUs = info.presentationTimeUs;
            /* render=true pushes the frame onto the AImageReader Surface */
            AMediaCodec_releaseOutputBuffer(d->vcodec, oi, info.size != 0);
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(d->vcodec);
            int32_t w = 0, h = 0;
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &w);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &h);
            if (w > 0 && h > 0) { d->vw = w; d->vh = h; }
            AMediaFormat_delete(f);
        } else {
            break; /* try again later / no buffer */
        }
    }
}

static void drain_audio_output(basis_decoder_t* d) {
    if (!d->acodec) return;
    for (;;) {
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(d->acodec, &info, 0);
        if (oi >= 0) {
            size_t cap = 0;
            uint8_t* buf = AMediaCodec_getOutputBuffer(d->acodec, oi, &cap);
            if (buf && info.size >= 2) {
                int n = info.size / 2; /* 16-bit PCM */
                float tmp[4096];
                const int16_t* s16 = (const int16_t*)(buf + info.offset);
                int off = 0;
                while (off < n) {
                    int chunk = n - off; if (chunk > 4096) chunk = 4096;
                    for (int i = 0; i < chunk; ++i) tmp[i] = s16[off + i] / 32768.0f;
                    ring_write(&d->pcm, tmp, chunk);
                    off += chunk;
                }
            }
            AMediaCodec_releaseOutputBuffer(d->acodec, oi, false);
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(d->acodec);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &d->asr);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &d->ach);
            AMediaFormat_delete(f);
        } else break;
    }
}

/* ---- URL path: extractor + worker thread -------------------------------- */

static AMediaCodec* create_codec_for_track(basis_decoder_t* d, AMediaFormat* fmt, int video) {
    const char* mime = NULL;
    if (!AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &mime) || !mime) return NULL;
    AMediaCodec* c = AMediaCodec_createDecoderByType(mime);
    if (!c) return NULL;
    ANativeWindow* surface = NULL;
    if (video) {
        int32_t w = 1280, h = 720;
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
        AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
        if (ensure_reader(d, w, h) != 0) { AMediaCodec_delete(c); return NULL; }
        surface = d->window;
    }
    if (AMediaCodec_configure(c, fmt, surface, NULL, 0) != AMEDIA_OK) { AMediaCodec_delete(c); return NULL; }
    if (AMediaCodec_start(c) != AMEDIA_OK) { AMediaCodec_delete(c); return NULL; }
    return c;
}

static void feed_extractor_sample(basis_decoder_t* d, AMediaCodec* codec, int track) {
    ssize_t ii = AMediaCodec_dequeueInputBuffer(codec, 0);
    if (ii < 0) return;
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(codec, ii, &cap);
    ssize_t sz = AMediaExtractor_readSampleData(d->extractor, buf, cap);
    if (sz < 0) {
        AMediaCodec_queueInputBuffer(codec, ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    } else {
        int64_t pts = AMediaExtractor_getSampleTime(d->extractor);
        AMediaCodec_queueInputBuffer(codec, ii, 0, sz, pts, 0);
        AMediaExtractor_advance(d->extractor);
    }
}

static void* url_worker(void* arg) {
    basis_decoder_t* d = (basis_decoder_t*)arg;
    basis_engine_set_state(d->engine, BASIS_MEDIA_STATE_PLAYING);
    while (basis_engine_is_running(d->engine)) {
        if (basis_engine_is_paused(d->engine)) { usleep(10000); continue; }

        int track = AMediaExtractor_getSampleTrackIndex(d->extractor);
        if (track == d->video_track && d->vcodec) feed_extractor_sample(d, d->vcodec, track);
        else if (track == d->audio_track && d->acodec) feed_extractor_sample(d, d->acodec, track);
        else if (track < 0) { usleep(5000); } /* EOS or none ready */
        else AMediaExtractor_advance(d->extractor);

        drain_video_output(d);
        drain_audio_output(d);
    }
    return NULL;
}

/* ---- internal API ------------------------------------------------------- */

basis_decoder_t* basis_decoder_create(basis_media_engine_t* engine) {
    basis_decoder_t* d = (basis_decoder_t*)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->engine = engine;
    d->video_track = d->audio_track = -1;
    d->lastPtsUs = -1;
    d->vk = basis_vk_create();
    ring_init(&d->pcm, 48000 * 2 * 4);
    return d;
}

void basis_decoder_destroy(basis_decoder_t* d) {
    if (!d) return;
    if (d->worker_started) pthread_join(d->worker, NULL);
    basis_decoder_render_release(d);
    if (d->vcodec) { AMediaCodec_stop(d->vcodec); AMediaCodec_delete(d->vcodec); }
    if (d->acodec) { AMediaCodec_stop(d->acodec); AMediaCodec_delete(d->acodec); }
    if (d->extractor) AMediaExtractor_delete(d->extractor);
    if (d->reader) AImageReader_delete(d->reader);
    if (d->vk) basis_vk_destroy(d->vk);
    ring_free(&d->pcm);
    free(d);
}

int basis_decoder_try_open_url(basis_decoder_t* d, const char* url) {
    /* Android: try AMediaExtractor first (HW demux of seekable containers).
     * On failure (live TS, HLS/DASH manifest, etc.) return 0 so the core falls
     * back to basis_jni_https + the portable basis_ts/basis_mp4 demuxer. */
    d->extractor = AMediaExtractor_new();
    media_status_t st = AMediaExtractor_setDataSource(d->extractor, url);
    if (st != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_INFO, "basis_media",
            "AMediaExtractor rejected url (code %d); falling back to JNI HTTPS + TS/MP4 demuxer", (int)st);
        AMediaExtractor_delete(d->extractor); d->extractor = NULL;
        return 0;
    }
    size_t n = AMediaExtractor_getTrackCount(d->extractor);
    for (size_t i = 0; i < n; ++i) {
        AMediaFormat* f = AMediaExtractor_getTrackFormat(d->extractor, i);
        const char* mime = NULL;
        AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime);
        if (mime && strncmp(mime, "video/", 6) == 0 && d->video_track < 0) {
            d->video_track = (int)i;
            AMediaExtractor_selectTrack(d->extractor, i);
            d->vcodec = create_codec_for_track(d, f, 1);
        } else if (mime && strncmp(mime, "audio/", 6) == 0 && d->audio_track < 0) {
            d->audio_track = (int)i;
            AMediaExtractor_selectTrack(d->extractor, i);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, &d->asr);
            AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &d->ach);
            d->acodec = create_codec_for_track(d, f, 0);
            d->aconfigured = 1;
        }
        AMediaFormat_delete(f);
    }
    if (!d->vcodec) { basis_engine_set_error(d->engine, "Android: no decodable video track"); return 1; }
    d->vconfigured = 1;

    pthread_create(&d->worker, NULL, url_worker, d);
    d->worker_started = 1;
    return 1; /* took ownership of the URL */
}

int basis_decoder_set_video_format(basis_decoder_t* d, basis_codec_t codec,
                                   const uint8_t* extradata, int extradata_len, int w, int h) {
    if (!d || d->vconfigured) return 0;
    d->vc = codec; if (w > 0) d->vw = w; if (h > 0) d->vh = h;
    const char* mime = (codec == BASIS_CODEC_H265) ? "video/hevc" : "video/avc";

    if (ensure_reader(d, d->vw ? d->vw : 1280, d->vh ? d->vh : 720) != 0) return -1;

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, d->vw ? d->vw : 1280);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, d->vh ? d->vh : 720);
    if (extradata && extradata_len > 0)
        AMediaFormat_setBuffer(fmt, "csd-0", (void*)extradata, extradata_len); /* Annex-B SPS/PPS(/VPS) */

    AMediaCodec* c = AMediaCodec_createDecoderByType(mime);
    if (!c || AMediaCodec_configure(c, fmt, d->window, NULL, 0) != AMEDIA_OK ||
        AMediaCodec_start(c) != AMEDIA_OK) {
        if (c) AMediaCodec_delete(c);
        AMediaFormat_delete(fmt);
        basis_engine_set_error(d->engine, "Android: video AMediaCodec configure/start failed");
        return -1;
    }
    d->vcodec = c;
    AMediaFormat_delete(fmt);
    d->vconfigured = 1;
    basis_engine_set_state(d->engine, BASIS_MEDIA_STATE_PLAYING);
    return 0;
}

int basis_decoder_set_audio_format(basis_decoder_t* d, basis_codec_t codec,
                                   int sample_rate, int channels, const uint8_t* asc, int asc_len) {
    if (!d || d->aconfigured || codec != BASIS_CODEC_AAC) return 0;
    d->asr = sample_rate; d->ach = channels;
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "audio/mp4a-latm");
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, sample_rate ? sample_rate : 48000);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels ? channels : 2);
    if (asc && asc_len > 0) AMediaFormat_setBuffer(fmt, "csd-0", (void*)asc, asc_len);
    AMediaCodec* c = AMediaCodec_createDecoderByType("audio/mp4a-latm");
    if (!c || AMediaCodec_configure(c, fmt, NULL, NULL, 0) != AMEDIA_OK ||
        AMediaCodec_start(c) != AMEDIA_OK) {
        if (c) AMediaCodec_delete(c);
        AMediaFormat_delete(fmt);
        d->aconfigured = 1;
        return -1;
    }
    d->acodec = c;
    AMediaFormat_delete(fmt);
    d->aconfigured = 1;
    return 0;
}

int basis_decoder_submit_video(basis_decoder_t* d, const uint8_t* annexb, int len, int64_t pts_us, int key) {
    (void)key;
    if (!d || !d->vcodec) return -1;
    ssize_t ii = AMediaCodec_dequeueInputBuffer(d->vcodec, 2000);
    if (ii >= 0) {
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(d->vcodec, ii, &cap);
        int n = len < (int)cap ? len : (int)cap;
        memcpy(buf, annexb, n);
        AMediaCodec_queueInputBuffer(d->vcodec, ii, 0, n, pts_us, 0);
    }
    drain_video_output(d);
    return 0;
}

int basis_decoder_submit_audio(basis_decoder_t* d, const uint8_t* data, int len, int64_t pts_us) {
    if (!d || !d->acodec) return -1;
    ssize_t ii = AMediaCodec_dequeueInputBuffer(d->acodec, 2000);
    if (ii >= 0) {
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(d->acodec, ii, &cap);
        int n = len < (int)cap ? len : (int)cap;
        memcpy(buf, data, n);
        AMediaCodec_queueInputBuffer(d->acodec, ii, 0, n, pts_us, 0);
    }
    drain_audio_output(d);
    return 0;
}

/* ---- render thread + accessors ----------------------------------------- */

int basis_decoder_render_update(basis_decoder_t* d) {
    if (!d || !d->vk) return -1;
    if (basis_engine_is_paused(d->engine)) return 0;
    return basis_vk_render_update(d->vk);
}
void basis_decoder_render_release(basis_decoder_t* d) { if (d && d->vk) basis_vk_release(d->vk); }

void* basis_decoder_get_texture(basis_decoder_t* d, int* w, int* h) {
    if (!d || !d->vk) { if (w) *w = 0; if (h) *h = 0; return NULL; }
    uint64_t img = basis_vk_get_image(d->vk, w, h);
    return (void*)(uintptr_t)img;
}
uint64_t basis_decoder_get_frame_counter(basis_decoder_t* d) { return d && d->vk ? basis_vk_frame_counter(d->vk) : 0; }
int basis_decoder_get_video_size(basis_decoder_t* d, int* w, int* h) {
    if (!d || d->vw <= 0) return -1; if (w) *w = d->vw; if (h) *h = d->vh; return 0;
}
/* The Vulkan resolve always flips to upright via a negative-height viewport, so
 * the published frame is bottom-left origin on every Android GPU. */
int basis_decoder_get_frame_origin(basis_decoder_t* d) { (void)d; return 0; }
int64_t basis_decoder_get_position_us(basis_decoder_t* d) { return d ? d->lastPtsUs : -1; }
int basis_decoder_get_audio_format(basis_decoder_t* d, int* r, int* c) {
    if (!d || !d->aconfigured) return -1; if (r) *r = d->asr ? d->asr : 48000; if (c) *c = d->ach ? d->ach : 2; return 0;
}
int basis_decoder_read_audio(basis_decoder_t* d, float* out, int max) { return d ? ring_read(&d->pcm, out, max) : 0; }
int basis_decoder_get_debug(basis_decoder_t* d, char* buf, int size) {
    if (!d || !buf || size <= 0) return 0;
    return snprintf(buf, (size_t)size, "vw=%d vh=%d", d->vw, d->vh);
}
void basis_decoder_set_buffer(basis_decoder_t* d, int mode, int ms) {
    /* Present pacing on the Android/Vulkan path is TODO (see basis_android_vk.c);
     * accept the call so the ABI is uniform. */
    (void)d; (void)mode; (void)ms;
}

void basis_decoder_set_output_texture(basis_decoder_t* d, void* native_texture, int w, int h) {
    if (d && d->vk) basis_vk_set_output_texture(d->vk, native_texture, w, h);
}
