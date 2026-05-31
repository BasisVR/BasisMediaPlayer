/* basis_hls.c — HLS / Low-Latency HLS source. See basis_hls.h for the contract.
 *
 * Model: parse the M3U8, select one rendition, and present the live media as a
 * single continuous byte stream by stitching segments (and, for LL-HLS, parts)
 * through a basis_read_fn that basis_ts_run / basis_mp4_run consume unchanged.
 *
 * This pass: clear streams, single rendition, Windows fetch. Android/Quest
 * support is planned (it needs a non-Windows http provider). */

#include "basis_hls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
  #include <pthread.h>
#endif

static void hls_sleep_ms(int ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* ---- portable mutex / thread (read-ahead producer) ---------------------- */
#if defined(_WIN32)
typedef CRITICAL_SECTION hls_mutex_t;
typedef HANDLE           hls_thread_t;
static void hls_mutex_init(hls_mutex_t* m)    { InitializeCriticalSection(m); }
static void hls_mutex_destroy(hls_mutex_t* m) { DeleteCriticalSection(m); }
static void hls_mutex_lock(hls_mutex_t* m)    { EnterCriticalSection(m); }
static void hls_mutex_unlock(hls_mutex_t* m)  { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t hls_mutex_t;
typedef pthread_t       hls_thread_t;
static void hls_mutex_init(hls_mutex_t* m)    { pthread_mutex_init(m, NULL); }
static void hls_mutex_destroy(hls_mutex_t* m) { pthread_mutex_destroy(m); }
static void hls_mutex_lock(hls_mutex_t* m)    { pthread_mutex_lock(m); }
static void hls_mutex_unlock(hls_mutex_t* m)  { pthread_mutex_unlock(m); }
#endif

static int64_t hls_now_us(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq; static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    int64_t f = freq.QuadPart ? freq.QuadPart : 1;
    /* split to avoid int64 overflow of counter*1e6 at large uptime (~256h @10MHz) */
    return (c.QuadPart / f) * 1000000LL + (c.QuadPart % f) * 1000000LL / f;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
#endif
}

#define HLS_MAX_URI       1024
#define HLS_MAX_ITEMS      512   /* fetchable items collected per playlist parse  */
#define HLS_MAX_PLAYLIST   (1 << 20) /* 1 MiB playlist cap                         */
#define HLS_MAX_EMPTY_RELOADS 8  /* consecutive no-new-media reloads before giving up */
#define HLS_LIVE_MARGIN_SEGMENTS 3 /* playout buffer kept behind the live edge for plain (non-LL) HLS */
#define HLS_RING_CAP (4 * 1024 * 1024) /* read-ahead byte buffer (~5 s of 1080p HD) */

/* (msn, part) media position. part == -1 means a whole segment. */
typedef struct {
    char uri[HLS_MAX_URI];
    long msn;
    int  part;   /* -1 = full segment, >=0 = partial segment index */
    long dur_ms; /* media duration of this segment/part (for real-time pacing) */
} hls_item_t;

typedef struct {
    long target_duration_ms;
    long media_seq_base;
    int  can_block_reload;   /* EXT-X-SERVER-CONTROL: CAN-BLOCK-RELOAD=YES   */
    long part_target_ms;     /* EXT-X-PART-INF: PART-TARGET                  */
    int  has_parts;          /* any EXT-X-PART seen                          */
    int  has_endlist;        /* EXT-X-ENDLIST (VOD / stream finished)        */
    int  is_fmp4;            /* EXT-X-MAP present or .m4s/.mp4 segment URIs  */
    char map_uri[HLS_MAX_URI]; /* EXT-X-MAP init segment (fMP4), resolved     */
    int  nfull;              /* count of full (#EXTINF) segments             */
    hls_item_t items[HLS_MAX_ITEMS];
    int  item_count;
} hls_playlist_t;

typedef struct basis_hls {
    basis_http_provider_t http;
    int  (*is_running)(void* user);
    void* user;

    char media_url[HLS_MAX_URI]; /* resolved media-playlist URL (reload target) */
    int  is_fmp4;

    /* media position cursor: the next thing we want to fetch */
    long want_msn;
    int  want_part;

    int  can_block_reload;
    long part_target_ms;
    long target_duration_ms;
    int  endlist_seen;

    int  map_served;             /* fMP4: init segment already streamed once */
    char map_uri[HLS_MAX_URI];

    /* pending fetch queue (resolved absolute URLs, in play order) */
    char pending[HLS_MAX_ITEMS][HLS_MAX_URI];
    long pending_dur[HLS_MAX_ITEMS];   /* media duration (ms) per queued item */
    int  pending_head;
    int  pending_count;

    void* seg_ctx;               /* current open segment byte source (producer only) */
    int   empty_reloads;

    /* read-ahead: the producer thread fetches segments into `ring`; basis_hls_read
     * drains it. Only `ring`/head/tail/count are shared (under `lock`); the queue,
     * seg_ctx and playlist cursor are touched by the producer thread only. */
    uint8_t*     ring;
    int          ring_cap;
    int          ring_head;      /* write position */
    int          ring_tail;      /* read position  */
    int          ring_count;     /* bytes currently buffered */
    hls_mutex_t  lock;
    hls_thread_t thread;
    int          thread_started;
    volatile int stop;
    volatile int producer_done;

    /* Real-time pacing: the decoder presents on a wall-clock and drops frames if
     * fed faster than real-time (it never back-pressures). So basis_hls_read meters
     * output to the stream's measured average bitrate via a token bucket. */
    volatile long target_bps;   /* measured average bytes/sec; 0 until known */
    long long     acc_bytes;    /* producer: cumulative segment bytes ... */
    long long     acc_dur_ms;   /* ... and their cumulative media duration */
    long          cur_seg_dur_ms;   /* duration of the segment the producer is on */
    long long     cur_seg_bytes;    /* bytes delivered for it so far */
    double        tb_tokens;    /* consumer token bucket (bytes) */
    int64_t       tb_last_us;   /* last token refill timestamp (0 = uninit) */
} basis_hls_t;

/* ---- small string / URL helpers ----------------------------------------- */

static int ci_eq_n(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

static int starts_with(const char* s, const char* p) {
    return strncmp(s, p, strlen(p)) == 0;
}

static int ends_with_ci(const char* s, const char* suffix) {
    size_t ls = strlen(s), lf = strlen(suffix);
    if (lf > ls) return 0;
    return ci_eq_n(s + (ls - lf), suffix, lf);
}

/* Extract a tag attribute value: KEY=VALUE or KEY="VALUE" from a tag line. */
static int attr_str(const char* line, const char* key, char* out, int outsz) {
    size_t klen = strlen(key);
    const char* p = line;
    while ((p = strstr(p, key)) != NULL) {
        /* require the char before key to be ':' or ',' to avoid substring hits */
        if (p != line && p[-1] != ':' && p[-1] != ',') { p += klen; continue; }
        if (p[klen] != '=') { p += klen; continue; }
        const char* v = p + klen + 1;
        int i = 0;
        if (*v == '"') {
            v++;
            while (*v && *v != '"' && i < outsz - 1) out[i++] = *v++;
        } else {
            while (*v && *v != ',' && *v != '\r' && *v != '\n' && i < outsz - 1) out[i++] = *v++;
        }
        out[i] = 0;
        return 1;
    }
    return 0;
}

static long attr_long(const char* line, const char* key, long def) {
    char buf[64];
    if (attr_str(line, key, buf, sizeof(buf))) return atol(buf);
    return def;
}

/* Seconds (possibly fractional, e.g. "0.33334") to milliseconds. */
static long attr_ms(const char* line, const char* key, long def) {
    char buf[64];
    if (!attr_str(line, key, buf, sizeof(buf))) return def;
    double s = atof(buf);
    return (long)(s * 1000.0 + 0.5);
}

/* Resolve `ref` against the absolute base URL `base` into `out`.
 * Handles absolute (http[s]://), root-relative (/path) and same-directory
 * relative refs. "../" is not normalised (rare in HLS); acceptable first pass. */
static void resolve_url(const char* base, const char* ref, char* out, int outsz) {
    if (starts_with(ref, "http://") || starts_with(ref, "https://")) {
        snprintf(out, outsz, "%s", ref);
        return;
    }
    /* find scheme://host end (first '/' after "scheme://") */
    const char* host = strstr(base, "://");
    host = host ? host + 3 : base;
    const char* host_end = strchr(host, '/');

    if (ref[0] == '/') {
        if (host_end) {
            int hlen = (int)(host - base) + (int)(host_end - host);
            snprintf(out, outsz, "%.*s%s", hlen, base, ref);
        } else {
            snprintf(out, outsz, "%s%s", base, ref);
        }
        return;
    }
    /* same-directory relative: base up to and including last '/' (before any '?') */
    const char* q = strchr(base, '?');
    const char* end = q ? q : base + strlen(base);
    const char* slash = end;
    while (slash > host && *slash != '/') slash--;
    int dirlen = (slash >= host && *slash == '/') ? (int)(slash - base) + 1 : (int)(end - base);
    snprintf(out, outsz, "%.*s%s", dirlen, base, ref);
}

/* ---- playlist fetch ------------------------------------------------------ */

/* GET `url` fully into a NUL-terminated buffer (caller frees). Returns length,
 * or <0 on error / stop. */
static int fetch_text(basis_hls_t* h, const char* url, char** out) {
    *out = NULL;
    void* ctx = h->http.open(url);
    if (!ctx) return -1;

    int cap = 16384, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { h->http.close(ctx); return -1; }

    for (;;) {
        if (h->is_running && !h->is_running(h->user)) { free(buf); h->http.close(ctx); return -1; }
        if (len + 4096 > cap) {
            if (cap >= HLS_MAX_PLAYLIST) break;
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); h->http.close(ctx); return -1; }
            buf = nb;
        }
        int n = h->http.read(ctx, (uint8_t*)buf + len, cap - len - 1);
        if (n <= 0) break;
        len += n;
    }
    h->http.close(ctx);
    buf[len] = 0;
    *out = buf;
    return len;
}

/* ---- M3U8 parsing -------------------------------------------------------- */

/* Returns 1 if the playlist is a master (has EXT-X-STREAM-INF). */
static int playlist_is_master(const char* text) {
    return strstr(text, "#EXT-X-STREAM-INF") != NULL;
}

/* Pick a single rendition from a master playlist: highest BANDWIDTH variant.
 * Writes its resolved absolute URL into out. Returns 1 on success. */
static int master_pick_variant(basis_hls_t* h, const char* base, const char* text,
                               char* out, int outsz) {
    const char* p = text;
    long best_bw = -1;
    char best_uri[HLS_MAX_URI] = {0};
    char line[2048];

    while (*p) {
        const char* nl = strchr(p, '\n');
        int llen = nl ? (int)(nl - p) : (int)strlen(p);
        if (llen >= (int)sizeof(line)) llen = (int)sizeof(line) - 1;
        memcpy(line, p, llen); line[llen] = 0;
        if (llen && line[llen - 1] == '\r') line[llen - 1] = 0;

        if (starts_with(line, "#EXT-X-STREAM-INF")) {
            long bw = attr_long(line, "BANDWIDTH", 0);
            /* the variant URI is the next non-comment, non-empty line */
            const char* q = nl ? nl + 1 : p + llen;
            while (*q) {
                const char* qnl = strchr(q, '\n');
                int qlen = qnl ? (int)(qnl - q) : (int)strlen(q);
                char uline[HLS_MAX_URI];
                if (qlen >= (int)sizeof(uline)) qlen = (int)sizeof(uline) - 1;
                memcpy(uline, q, qlen); uline[qlen] = 0;
                if (qlen && uline[qlen - 1] == '\r') uline[qlen - 1] = 0;
                if (uline[0] && uline[0] != '#') {
                    if (bw >= best_bw) { best_bw = bw; resolve_url(base, uline, best_uri, sizeof(best_uri)); }
                    break;
                }
                if (!qnl) break;
                q = qnl + 1;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }

    if (!best_uri[0]) return 0;
    snprintf(out, outsz, "%s", best_uri);
    return 1;
}

/* Parse a media playlist into pl. base = media playlist URL (for resolving). */
static void parse_media_playlist(const char* base, const char* text, hls_playlist_t* pl) {
    memset(pl, 0, sizeof(*pl));
    pl->media_seq_base = 0;

    const char* p = text;
    char line[2048];
    int seg_index = 0;   /* full segments seen so far */
    int part_index = 0;  /* parts of the in-progress segment */

    while (*p) {
        const char* nl = strchr(p, '\n');
        int llen = nl ? (int)(nl - p) : (int)strlen(p);
        if (llen >= (int)sizeof(line)) llen = (int)sizeof(line) - 1;
        memcpy(line, p, llen); line[llen] = 0;
        if (llen && line[llen - 1] == '\r') line[llen - 1] = 0;

        if (starts_with(line, "#EXT-X-TARGETDURATION")) {
            const char* c = strchr(line, ':');
            if (c) pl->target_duration_ms = atol(c + 1) * 1000;
        } else if (starts_with(line, "#EXT-X-MEDIA-SEQUENCE")) {
            const char* c = strchr(line, ':');
            if (c) pl->media_seq_base = atol(c + 1);
        } else if (starts_with(line, "#EXT-X-SERVER-CONTROL")) {
            char v[16];
            if (attr_str(line, "CAN-BLOCK-RELOAD", v, sizeof(v)) && ci_eq_n(v, "yes", 3))
                pl->can_block_reload = 1;
        } else if (starts_with(line, "#EXT-X-PART-INF")) {
            pl->part_target_ms = attr_ms(line, "PART-TARGET", pl->part_target_ms);
        } else if (starts_with(line, "#EXT-X-MAP")) {
            char uri[HLS_MAX_URI];
            if (attr_str(line, "URI", uri, sizeof(uri))) {
                resolve_url(base, uri, pl->map_uri, sizeof(pl->map_uri));
                pl->is_fmp4 = 1;
            }
        } else if (starts_with(line, "#EXT-X-PART:")) {
            char uri[HLS_MAX_URI];
            if (attr_str(line, "URI", uri, sizeof(uri)) && pl->item_count < HLS_MAX_ITEMS) {
                hls_item_t* it = &pl->items[pl->item_count++];
                resolve_url(base, uri, it->uri, sizeof(it->uri));
                it->msn = pl->media_seq_base + seg_index;
                it->part = part_index++;
                it->dur_ms = attr_ms(line, "DURATION", 0);
                pl->has_parts = 1;
                if (ends_with_ci(it->uri, ".m4s") || ends_with_ci(it->uri, ".mp4")) pl->is_fmp4 = 1;
            }
        } else if (starts_with(line, "#EXTINF")) {
            /* #EXTINF:<seconds>,  — capture the duration for real-time pacing */
            long extinf_ms = 0;
            { const char* c = strchr(line, ':'); if (c) extinf_ms = (long)(atof(c + 1) * 1000.0 + 0.5); }
            /* the segment URI is the next non-comment line */
            const char* q = nl ? nl + 1 : p + llen;
            while (*q) {
                const char* qnl = strchr(q, '\n');
                int qlen = qnl ? (int)(qnl - q) : (int)strlen(q);
                char uline[HLS_MAX_URI];
                if (qlen >= (int)sizeof(uline)) qlen = (int)sizeof(uline) - 1;
                memcpy(uline, q, qlen); uline[qlen] = 0;
                if (qlen && uline[qlen - 1] == '\r') uline[qlen - 1] = 0;
                if (uline[0] && uline[0] != '#') {
                    if (pl->item_count < HLS_MAX_ITEMS) {
                        hls_item_t* it = &pl->items[pl->item_count++];
                        resolve_url(base, uline, it->uri, sizeof(it->uri));
                        it->msn = pl->media_seq_base + seg_index;
                        it->part = -1;
                        it->dur_ms = extinf_ms;
                        if (ends_with_ci(it->uri, ".m4s") || ends_with_ci(it->uri, ".mp4")) pl->is_fmp4 = 1;
                    }
                    seg_index++;
                    part_index = 0; /* parts now belong to the next in-progress segment */
                    p = qnl ? qnl : q + qlen;
                    goto next_line;
                }
                if (!qnl) { p = q + qlen; goto next_line; }
                q = qnl + 1;
            }
        } else if (starts_with(line, "#EXT-X-ENDLIST")) {
            pl->has_endlist = 1;
        }

        if (!nl) break;
        p = nl + 1;
        continue;
    next_line:
        if (!*p) break;
        if (*p == '\n') p++;
        continue;
    }

    pl->nfull = seg_index;
}

/* ---- queue helpers ------------------------------------------------------- */

static void queue_push(basis_hls_t* h, const char* url, long dur_ms) {
    if (h->pending_count >= HLS_MAX_ITEMS) return;
    int idx = (h->pending_head + h->pending_count) % HLS_MAX_ITEMS;
    snprintf(h->pending[idx], HLS_MAX_URI, "%s", url);
    h->pending_dur[idx] = dur_ms;
    h->pending_count++;
}

static const char* queue_pop(basis_hls_t* h, long* out_dur_ms) {
    if (h->pending_count == 0) return NULL;
    int idx = h->pending_head;
    const char* s = h->pending[idx];
    if (out_dur_ms) *out_dur_ms = h->pending_dur[idx];
    h->pending_head = (h->pending_head + 1) % HLS_MAX_ITEMS;
    h->pending_count--;
    return s;
}

/* Enqueue everything in the freshly parsed playlist that is at or beyond our
 * (want_msn, want_part) cursor, advancing the cursor past what we enqueue. A full
 * segment for msn M supersedes its parts: if we already consumed parts of M we
 * skip the full segment, otherwise we take it. */
static void enqueue_new_media(basis_hls_t* h, const hls_playlist_t* pl) {
    for (int i = 0; i < pl->item_count; ++i) {
        const hls_item_t* it = &pl->items[i];
        if (it->part >= 0) {
            /* part P of segment M */
            if (it->msn > h->want_msn || (it->msn == h->want_msn && it->part >= h->want_part)) {
                queue_push(h, it->uri, it->dur_ms);
                h->want_msn = it->msn;
                h->want_part = it->part + 1;
            }
        } else {
            /* full segment M completes that segment */
            if (h->want_msn < it->msn) {
                queue_push(h, it->uri, it->dur_ms);
                h->want_msn = it->msn + 1;
                h->want_part = 0;
            } else if (h->want_msn == it->msn && h->want_part == 0) {
                queue_push(h, it->uri, it->dur_ms);
                h->want_msn = it->msn + 1;
                h->want_part = 0;
            } else if (h->want_msn == it->msn && h->want_part > 0) {
                /* already rode this segment's parts; skip the redundant full segment */
                h->want_msn = it->msn + 1;
                h->want_part = 0;
            }
        }
    }
}

/* Build the (optionally blocking) reload URL and fetch+parse the media playlist,
 * enqueuing any new media. Returns 1 if new media was enqueued, 0 if none, <0 on
 * error/stop. */
static int reload_and_enqueue(basis_hls_t* h) {
    char url[HLS_MAX_URI + 96];
    if (h->can_block_reload && h->want_part >= 0) {
        char sep = strchr(h->media_url, '?') ? '&' : '?';
        snprintf(url, sizeof(url), "%s%c_HLS_msn=%ld&_HLS_part=%d",
                 h->media_url, sep, h->want_msn, h->want_part);
    } else {
        snprintf(url, sizeof(url), "%s", h->media_url);
    }

    char* text = NULL;
    int n = fetch_text(h, url, &text);
    if (n < 0) { free(text); return -1; }

    hls_playlist_t pl;
    parse_media_playlist(h->media_url, text, &pl);
    free(text);

    if (pl.target_duration_ms) h->target_duration_ms = pl.target_duration_ms;
    if (pl.has_endlist) h->endlist_seen = 1;

    int before = h->pending_count;
    enqueue_new_media(h, &pl);
    return (h->pending_count > before) ? 1 : 0;
}

/* ---- read-ahead producer ------------------------------------------------- */

static int hls_should_run(basis_hls_t* h) {
    return !h->stop && (h->is_running == NULL || h->is_running(h->user));
}

/* Copy n bytes into the ring, sleeping while it's full. Stops early on shutdown. */
static void ring_write(basis_hls_t* h, const uint8_t* data, int n) {
    int written = 0;
    while (written < n && hls_should_run(h)) {
        hls_mutex_lock(&h->lock);
        int space = h->ring_cap - h->ring_count;
        if (space > 0) {
            int chunk = n - written;
            if (chunk > space) chunk = space;
            int first = h->ring_cap - h->ring_head;
            if (first > chunk) first = chunk;
            memcpy(h->ring + h->ring_head, data + written, (size_t)first);
            if (chunk > first) memcpy(h->ring, data + written + first, (size_t)(chunk - first));
            h->ring_head = (h->ring_head + chunk) % h->ring_cap;
            h->ring_count += chunk;
            hls_mutex_unlock(&h->lock);
            written += chunk;
        } else {
            hls_mutex_unlock(&h->lock);
            hls_sleep_ms(2); /* ring full — wait for the consumer to drain */
        }
    }
}

/* Producer loop: fetch segments/parts (and reload the live playlist) ahead of
 * playout into the ring, so the decoder sees a continuous byte stream with no
 * per-segment connection gaps. */
static void hls_producer(basis_hls_t* h) {
    uint8_t tmp[16384];
    while (hls_should_run(h)) {
        if (!h->seg_ctx) {
            if (h->is_fmp4 && !h->map_served && h->map_uri[0]) {
                h->seg_ctx = h->http.open(h->map_uri); /* fMP4 init segment first */
                h->map_served = 1;
                h->cur_seg_dur_ms = 0; h->cur_seg_bytes = 0; /* init segment: not counted */
                if (!h->seg_ctx) continue;
            } else {
                const char* next = queue_pop(h, &h->cur_seg_dur_ms);
                if (next) {
                    h->cur_seg_bytes = 0;
                    h->seg_ctx = h->http.open(next);
                    if (!h->seg_ctx) continue; /* skip a transient open failure */
                    h->empty_reloads = 0;
                } else if (h->endlist_seen) {
                    break; /* VOD / stream finished */
                } else {
                    int r = reload_and_enqueue(h);
                    if (r > 0) { h->empty_reloads = 0; }
                    else if (r < 0) {
                        if (!hls_should_run(h)) break;
                        hls_sleep_ms(50); /* transient fetch error — back off and retry */
                    } else { /* r == 0: nothing new yet */
                        if (++h->empty_reloads >= HLS_MAX_EMPTY_RELOADS) break;
                        if (!h->can_block_reload) {
                            long wait = h->target_duration_ms > 0 ? h->target_duration_ms / 2 : 1000, w = 0;
                            while (w < wait && hls_should_run(h)) { hls_sleep_ms(50); w += 50; }
                        }
                    }
                    continue;
                }
            }
        }

        int n = h->http.read(h->seg_ctx, tmp, (int)sizeof(tmp));
        if (n > 0) {
            h->cur_seg_bytes += n;
            ring_write(h, tmp, n);
        } else {
            h->http.close(h->seg_ctx);
            h->seg_ctx = NULL;
            /* fold this segment into the running average bitrate that paces output */
            if (h->cur_seg_dur_ms > 0) {
                h->acc_bytes += h->cur_seg_bytes;
                h->acc_dur_ms += h->cur_seg_dur_ms;
                if (h->acc_dur_ms > 0)
                    h->target_bps = (long)(h->acc_bytes * 1000 / h->acc_dur_ms);
            }
            h->cur_seg_bytes = 0; h->cur_seg_dur_ms = 0;
            /* top up the queue in the background so the next segment is ready */
            if (!h->endlist_seen && h->pending_count <= HLS_LIVE_MARGIN_SEGMENTS)
                reload_and_enqueue(h);
        }
    }
    h->producer_done = 1;
}

#if defined(_WIN32)
static DWORD WINAPI hls_thread_entry(LPVOID arg) { hls_producer((basis_hls_t*)arg); return 0; }
#else
static void* hls_thread_entry(void* arg) { hls_producer((basis_hls_t*)arg); return NULL; }
#endif

static int hls_thread_start(basis_hls_t* h) {
#if defined(_WIN32)
    h->thread = CreateThread(NULL, 0, hls_thread_entry, h, 0, NULL);
    return h->thread != NULL;
#else
    return pthread_create(&h->thread, NULL, hls_thread_entry, h) == 0;
#endif
}

static void hls_thread_join(basis_hls_t* h) {
    if (!h->thread_started) return;
#if defined(_WIN32)
    WaitForSingleObject(h->thread, INFINITE);
    CloseHandle(h->thread);
#else
    pthread_join(h->thread, NULL);
#endif
    h->thread_started = 0;
}

/* ---- public API ---------------------------------------------------------- */

void* basis_hls_open(const char* url, const basis_http_provider_t* http,
                     int (*is_running)(void* user), void* user, int* out_is_fmp4) {
    if (!url || !http || !http->open || !http->read || !http->close) return NULL;

    basis_hls_t* h = (basis_hls_t*)calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->http = *http;
    h->is_running = is_running;
    h->user = user;

    /* Fetch the entry playlist; follow one master->media indirection. */
    char* text = NULL;
    if (fetch_text(h, url, &text) < 0 || !text) { free(text); free(h); return NULL; }

    if (playlist_is_master(text)) {
        char media[HLS_MAX_URI];
        if (!master_pick_variant(h, url, text, media, sizeof(media))) { free(text); free(h); return NULL; }
        snprintf(h->media_url, sizeof(h->media_url), "%s", media);
        free(text);
        if (fetch_text(h, h->media_url, &text) < 0 || !text) { free(text); free(h); return NULL; }
    } else {
        snprintf(h->media_url, sizeof(h->media_url), "%s", url);
    }

    hls_playlist_t pl;
    parse_media_playlist(h->media_url, text, &pl);
    free(text);

    if (pl.nfull == 0 && !pl.has_parts) { free(h); return NULL; }

    h->is_fmp4 = pl.is_fmp4;
    h->can_block_reload = pl.can_block_reload && pl.has_parts;
    h->part_target_ms = pl.part_target_ms;
    h->target_duration_ms = pl.target_duration_ms ? pl.target_duration_ms : 6000;
    h->endlist_seen = pl.has_endlist;
    if (pl.map_uri[0]) snprintf(h->map_uri, sizeof(h->map_uri), "%s", pl.map_uri);

    /* Start at the live edge. LL: the in-progress segment's first part (lowest
     * latency; first part of a segment is normally an independent keyframe).
     * Non-LL: the last complete segment (guaranteed keyframe). */
    if (h->can_block_reload) {
        h->want_msn = pl.media_seq_base + pl.nfull;
        h->want_part = 0;
    } else {
        /* Start a few segments behind the live edge so playout always has a buffer and
         * segment fetches never wait on the encoder (plain HLS has no parts to ride). Each
         * segment starts on a keyframe, so any of these is a valid decode start. */
        long edge = pl.media_seq_base + (pl.nfull > 0 ? pl.nfull - 1 : 0);
        long start = edge - HLS_LIVE_MARGIN_SEGMENTS;
        if (start < pl.media_seq_base) start = pl.media_seq_base;
        h->want_msn = start;
        h->want_part = 0;
    }

    enqueue_new_media(h, &pl);

    /* Start the read-ahead producer so segments buffer ahead of playout. */
    h->ring_cap = HLS_RING_CAP;
    h->ring = (uint8_t*)malloc((size_t)h->ring_cap);
    if (!h->ring) { free(h); return NULL; }
    hls_mutex_init(&h->lock);
    if (!hls_thread_start(h)) {
        hls_mutex_destroy(&h->lock);
        free(h->ring);
        free(h);
        return NULL;
    }
    h->thread_started = 1;

    if (out_is_fmp4) *out_is_fmp4 = h->is_fmp4;
    return h;
}

int basis_hls_read(void* ctx, uint8_t* buf, int len) {
    basis_hls_t* h = (basis_hls_t*)ctx;
    if (!h || len <= 0) return 0;

    for (;;) {
        if (h->stop || (h->is_running && !h->is_running(h->user))) return 0;

        /* Real-time pacing: the decoder presents on a wall-clock and drops frames
         * if fed faster than real-time, so meter output to the measured average
         * bitrate via a token bucket (≈0.25 s burst). Before the rate is known
         * (first segment), serve unthrottled. */
        int budget = len;
        long rate = h->target_bps;
        if (rate > 0) {
            int64_t now = hls_now_us();
            if (h->tb_last_us == 0) h->tb_last_us = now;
            h->tb_tokens += (double)rate * (double)(now - h->tb_last_us) / 1000000.0;
            h->tb_last_us = now;
            /* ~0.5 s burst: enough to deliver a segment-start keyframe (which is several
             * frames' worth of bytes) promptly so `newest` doesn't stall and the present
             * buffer doesn't dip — still ≤ the decoder's ~533 ms present ring, so it can't
             * lap/flood. Steady rate stays at the average bitrate. */
            double burst = (double)rate * 0.5;
            if (h->tb_tokens > burst) h->tb_tokens = burst;
            if (h->tb_tokens < (double)budget) budget = (int)h->tb_tokens;
        }

        if (budget > 0) {
            hls_mutex_lock(&h->lock);
            if (h->ring_count > 0) {
                int take = h->ring_count < budget ? h->ring_count : budget;
                int first = h->ring_cap - h->ring_tail;
                if (first > take) first = take;
                memcpy(buf, h->ring + h->ring_tail, (size_t)first);
                if (take > first) memcpy(buf + first, h->ring, (size_t)(take - first));
                h->ring_tail = (h->ring_tail + take) % h->ring_cap;
                h->ring_count -= take;
                hls_mutex_unlock(&h->lock);
                if (rate > 0) h->tb_tokens -= take;
                return take;
            }
            int done = h->producer_done;
            hls_mutex_unlock(&h->lock);
            if (done) return 0;  /* producer finished and ring drained -> end of stream */
        }

        hls_sleep_ms(2); /* rate-limited, or waiting for the producer to buffer */
    }
}

void basis_hls_close(void* ctx) {
    basis_hls_t* h = (basis_hls_t*)ctx;
    if (!h) return;
    h->stop = 1;
    hls_thread_join(h);
    if (h->seg_ctx) { h->http.close(h->seg_ctx); h->seg_ctx = NULL; }
    hls_mutex_destroy(&h->lock);
    free(h->ring);
    free(h);
}
