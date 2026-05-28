/* Blocking TCP client sockets (Winsock / BSD). No TLS — plaintext rtsp/rtmp and
 * plaintext http only. TLS streams use the platform stacks (WinHTTP on Windows,
 * AMediaExtractor on Android). */
#ifndef BASIS_IO_H
#define BASIS_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct basis_io basis_io_t;

/* Connect with a timeout (ms). Returns NULL on failure. */
basis_io_t* basis_io_connect(const char* host, int port, int timeout_ms);

/* Returns bytes read (>0), 0 on orderly close, <0 on error. */
int basis_io_read(basis_io_t* io, uint8_t* buf, int len);

/* Reads exactly `len` bytes unless the connection closes/errors first.
 * Returns len on success, <len on close/error. */
int basis_io_read_full(basis_io_t* io, uint8_t* buf, int len);

/* Writes exactly `len` bytes. Returns len on success, <0 on error. */
int basis_io_write_full(basis_io_t* io, const uint8_t* buf, int len);

/* Sets the per-recv timeout in ms (0 = block forever). */
void basis_io_set_read_timeout(basis_io_t* io, int timeout_ms);

void basis_io_close(basis_io_t* io);

/* Process-wide one-time init/teardown (WSAStartup on Windows; no-op elsewhere). */
void basis_io_global_init(void);
void basis_io_global_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
