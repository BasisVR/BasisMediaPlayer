/* WinHTTP byte source for http:// and https:// live streams on Windows.
 * Handles TLS, redirects and chunked transfer transparently; the demuxers just
 * pull bytes via basis_win_http_read (basis_read_fn-compatible). */
#ifndef BASIS_WIN_HTTP_H
#define BASIS_WIN_HTTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* basis_win_http_open(const char* url);
int   basis_win_http_read(void* ctx, uint8_t* buf, int len);
void  basis_win_http_close(void* ctx);

/* Unblock a thread parked in basis_win_http_read by closing the underlying request, so a
 * pending read returns immediately. Call before joining a reader thread on shutdown so the
 * join can't stall on a stalled socket read; a following basis_win_http_close stays safe. */
void  basis_win_http_abort(void* ctx);

/* 1 if the response is a finite, byte-range-seekable body (known Content-Length and
 * Accept-Ranges: bytes) — i.e. on-demand/VOD rather than an open-ended live stream.
 * 0 otherwise. Reflects the headers captured when the source was opened. */
int   basis_win_http_is_seekable(void* ctx);

#ifdef __cplusplus
}
#endif
#endif
