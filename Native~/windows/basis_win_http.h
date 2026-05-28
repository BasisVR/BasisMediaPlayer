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

#ifdef __cplusplus
}
#endif
#endif
