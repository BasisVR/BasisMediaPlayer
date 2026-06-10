#include "basis_rist.h"

/* RIST Main-Profile receiver, built only when BASIS_WITH_RIST is defined (it links
 * the prebuilt librist static, which vendors its own mbedTLS). With the flag off
 * (the default) only the graceful "not built" stub below is built and the plugin
 * links exactly as before — no new dependencies.
 *
 * Implemented against the librist 0.2.x receiver API. librist owns the UDP sockets,
 * ARQ, GRE tunnel, jitter buffer and (Main Profile) PSK-AES, and hands the recovered
 * MPEG-TS payload to the data callback; we buffer it and expose it as a basis_read_fn
 * so basis_ts_run consumes RIST exactly as it consumes the TCP/HTTP byte sources.
 *
 * Open items (not blockers for the byte path): HE-AAC coverage is a decoder-side
 * concern, and the librist error text mapped onto the sink is coarse. */

#ifdef BASIS_WITH_RIST

#include <librist/librist.h>
#include <librist/receiver.h>
#include <librist/peer.h>
#include <librist/logging.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
  #include <windows.h>
  typedef CRITICAL_SECTION rist_mutex_t;
  static void rmx_init(rist_mutex_t* m)   { InitializeCriticalSection(m); }
  static void rmx_destroy(rist_mutex_t* m){ DeleteCriticalSection(m); }
  static void rmx_lock(rist_mutex_t* m)   { EnterCriticalSection(m); }
  static void rmx_unlock(rist_mutex_t* m) { LeaveCriticalSection(m); }
  static void rist_sleep_ms(int ms)       { Sleep((DWORD)ms); }
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_mutex_t rist_mutex_t;
  static void rmx_init(rist_mutex_t* m)   { pthread_mutex_init(m, NULL); }
  static void rmx_destroy(rist_mutex_t* m){ pthread_mutex_destroy(m); }
  static void rmx_lock(rist_mutex_t* m)   { pthread_mutex_lock(m); }
  static void rmx_unlock(rist_mutex_t* m) { pthread_mutex_unlock(m); }
  static void rist_sleep_ms(int ms)       { usleep((useconds_t)ms * 1000); }
#endif

/* Single hand-off point between librist's internal threads (which write recovered
 * TS) and the demux thread (which reads it through basis_rist_read). Allocated
 * once at open; no per-read allocation. Overwrites oldest bytes if the demuxer
 * falls behind — librist has already done ARQ recovery upstream, so an overrun
 * here means the ring is undersized, not a network loss. */
#define BASIS_RIST_RING_BYTES (1 << 20)  /* 1 MiB of recovered TS in flight */

typedef struct {
    uint8_t*     buf;
    size_t       cap;
    size_t       head;   /* write index */
    size_t       tail;   /* read index */
    size_t       fill;   /* bytes available */
    rist_mutex_t lock;
    volatile int shutdown;
} basis_rist_ring;

static int ring_init(basis_rist_ring* r, size_t cap) {
    r->buf = (uint8_t*)malloc(cap);
    if (!r->buf) return -1;
    r->cap = cap; r->head = r->tail = r->fill = 0; r->shutdown = 0;
    rmx_init(&r->lock);
    return 0;
}
static void ring_free(basis_rist_ring* r) {
    if (r->buf) { free(r->buf); r->buf = NULL; }
    rmx_destroy(&r->lock);
}
static void ring_write(basis_rist_ring* r, const uint8_t* data, size_t len) {
    if (len == 0) return;
    rmx_lock(&r->lock);
    for (size_t i = 0; i < len; ++i) {
        r->buf[r->head] = data[i];
        r->head = (r->head + 1) % r->cap;
        if (r->fill < r->cap) r->fill++;
        else r->tail = (r->tail + 1) % r->cap;  /* full: drop oldest */
    }
    rmx_unlock(&r->lock);
}
/* Non-blocking drain of up to `len` bytes; returns bytes copied (may be 0). */
static int ring_drain(basis_rist_ring* r, uint8_t* out, int len) {
    int n = 0;
    rmx_lock(&r->lock);
    while (n < len && r->fill > 0) {
        out[n++] = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        r->fill--;
    }
    rmx_unlock(&r->lock);
    return n;
}

typedef struct basis_rist_ctx {
    struct rist_ctx*              receiver;   /* librist handle */
    struct rist_logging_settings* log;        /* librist's init logs through this; NULL crashes on Windows */
    basis_rist_ring               ring;
    basis_media_sink_t*           sink;
} basis_rist_ctx;

/* librist data callback — runs on a librist output thread, must be thread-safe and
 * must not stall. librist delivers the recovered MPEG-TS payload (de-RTP'd,
 * de-jittered, decrypted), so it feeds basis_ts_run unchanged. The block is
 * reference-counted and must be released with rist_receiver_data_block_free2. */
static int basis_rist_on_data(void* arg, struct rist_data_block* block) {
    basis_rist_ctx* c = (basis_rist_ctx*)arg;
    if (block && block->payload && block->payload_len)
        ring_write(&c->ring, (const uint8_t*)block->payload, block->payload_len);
    rist_receiver_data_block_free2(&block);
    return 0;
}

void* basis_rist_open(const basis_url_t* parts, basis_media_sink_t* sink) {
    basis_rist_ctx* c = (basis_rist_ctx*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->sink = sink;
    if (ring_init(&c->ring, BASIS_RIST_RING_BYTES) != 0) { free(c); return NULL; }

    /* Reconstruct the rist:// URL for librist's parser. It wants
     * "rist://host:port[?query]" with NO path — a trailing '/' makes it mis-parse the
     * host/port entirely. parts->path holds the query (with any leading '/'), so pass
     * only the part from '?' onward; secret / aes-type / buffer ride in that query. */
    char url[2304];
    const char* query = strchr(parts->path, '?');
    snprintf(url, sizeof(url), "rist://%s:%d%s", parts->host, parts->port, query ? query : "");

    /* librist's init path logs through a logging-settings object plus a global log
     * mutex. Passing NULL leaves that mutex uninitialised, and librist's bundled
     * Windows pthread shim doesn't lazily init it (glibc does), so the first
     * internal log call faults. Create real (disabled) settings first — this also
     * initialises librist's global logging state — then hand them to the receiver. */
    if (rist_logging_set(&c->log, RIST_LOG_DISABLE, NULL, NULL, NULL, NULL) != 0 || c->log == NULL) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: failed to init logging settings.");
        basis_rist_close(c);
        return NULL;
    }

    if (rist_receiver_create(&c->receiver, RIST_PROFILE_MAIN, c->log) != 0) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: failed to create receiver.");
        basis_rist_close(c);
        return NULL;
    }

    /* librist parses host/port plus encryption (secret/aes-type) and buffer depth
     * from the URL query into the peer config. */
    struct rist_peer_config* peer_cfg = NULL;
    if (rist_parse_address2(url, &peer_cfg) < 0 || peer_cfg == NULL) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: invalid address or parameters.");
        basis_rist_close(c);
        return NULL;
    }
    peer_cfg->initiate_conn = 1;  /* caller: open an outbound flow to the broadcaster */

    struct rist_peer* peer = NULL;
    int peer_rc = rist_peer_create(c->receiver, &peer, peer_cfg);
    rist_peer_config_free2(&peer_cfg);
    if (peer_rc != 0) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: failed to add peer.");
        basis_rist_close(c);
        return NULL;
    }

    if (rist_receiver_data_callback_set2(c->receiver, basis_rist_on_data, c) != 0) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: failed to set data callback.");
        basis_rist_close(c);
        return NULL;
    }

    if (rist_start(c->receiver) != 0) {
        if (sink && sink->on_error) sink->on_error(sink->user, "RIST: failed to start receiver.");
        basis_rist_close(c);
        return NULL;
    }
    return c;
}

int basis_rist_read(void* ctx, uint8_t* buf, int len) {
    basis_rist_ctx* c = (basis_rist_ctx*)ctx;
    if (!c || len <= 0) return -1;
    /* Block until data is available or shutdown, polling so the demux thread
     * never wedges on a dead receiver (matches the engine's "pull, never block
     * forever" contract). */
    for (;;) {
        if (c->ring.shutdown) return 0;
        if (c->sink && c->sink->is_running && !c->sink->is_running(c->sink->user)) return 0;
        int n = ring_drain(&c->ring, buf, len);
        if (n > 0) return n;
        rist_sleep_ms(5);
    }
}

void basis_rist_close(void* ctx) {
    basis_rist_ctx* c = (basis_rist_ctx*)ctx;
    if (!c) return;
    c->ring.shutdown = 1;
    if (c->receiver) { rist_destroy(c->receiver); c->receiver = NULL; }
    if (c->log) { rist_logging_settings_free2(&c->log); }  /* after the receiver that used it */
    ring_free(&c->ring);
    free(c);
}

#else  /* !BASIS_WITH_RIST — graceful stub so the plugin links with no new deps */

void* basis_rist_open(const basis_url_t* parts, basis_media_sink_t* sink) {
    (void)parts;
    if (sink && sink->on_error)
        sink->on_error(sink->user,
            "RIST is not built into this plugin. Rebuild basis_media_native with "
            "-DBASIS_WITH_RIST=ON and the vendored librist static.");
    return NULL;
}
int  basis_rist_read(void* ctx, uint8_t* buf, int len) { (void)ctx; (void)buf; (void)len; return -1; }
void basis_rist_close(void* ctx) { (void)ctx; }

#endif /* BASIS_WITH_RIST */
