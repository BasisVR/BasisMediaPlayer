/* HLS / Low-Latency HLS source.
 *
 * Not a demuxer: this fetches and parses the M3U8 playlist, selects a single
 * rendition, and exposes the live media as ONE continuous byte stream
 * (basis_read_fn-compatible) by stitching segments — and, for LL-HLS, partial
 * segments — back to back. The existing MPEG-TS (basis_ts_run) and fMP4
 * (basis_mp4_run) demuxers then consume it unchanged.
 *
 * Transport is injected as a basis_http_provider so the protocol code stays
 * portable: on Windows the engine passes the basis_win_http_* trio (TLS, redirects
 * and chunked handled there). Playlists and segments are fetched through it.
 *
 * Low latency: when the origin advertises EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD
 * with EXT-X-PART parts, the client starts near the live edge, reloads the media
 * playlist with blocking _HLS_msn/_HLS_part queries, and feeds parts as they are
 * produced — targeting ~PART-HOLD-BACK latency. Against a plain (non-LL) origin it
 * falls back to live-edge, segment-by-segment playback (segment-bound latency).
 *
 * Scope: clear streams, single rendition, Windows fetch. Android/Quest planned.
 */
#ifndef BASIS_HLS_H
#define BASIS_HLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pluggable HTTP(S) byte-source. Matches basis_win_http_open/read/close exactly:
 * open(url) returns a context that streams the response body; read is
 * basis_read_fn-compatible (bytes read, 0 on EOF, <0 on error); close frees it. */
typedef struct basis_http_provider {
    void* (*open)(const char* url);
    int   (*read)(void* ctx, uint8_t* buf, int len);
    void  (*close)(void* ctx);
} basis_http_provider_t;

/* Open an HLS source. Fetches the (master and/or media) playlist via `http`,
 * resolves a single rendition, and prepares the stitched live byte stream.
 * `is_running(user)` is polled during blocking reloads/retries so a stop unwinds
 * promptly. On success returns a context and sets *out_is_fmp4 to 1 when segments
 * are fragmented-MP4 (feed basis_mp4_run) or 0 for MPEG-TS (feed basis_ts_run).
 * Returns NULL on failure. */
void* basis_hls_open(const char* url, const basis_http_provider_t* http,
                     int (*is_running)(void* user), void* user, int* out_is_fmp4);

/* basis_read_fn-compatible. Serves the stitched segment/part bytes, advancing to
 * the next segment and reloading the playlist (blocking for LL-HLS) as needed.
 * Returns bytes read, 0 when the stream ends or the engine stops, <0 on error. */
int basis_hls_read(void* ctx, uint8_t* buf, int len);

void basis_hls_close(void* ctx);

#ifdef __cplusplus
}
#endif
#endif
