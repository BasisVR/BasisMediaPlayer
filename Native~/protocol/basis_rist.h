/* RIST (Reliable Internet Stream Transport) Main-Profile receiver.
 *
 * RIST is a transport, not a container: it carries an MPEG-TS payload over UDP
 * with ARQ retransmission (and, in Main Profile, a GRE tunnel + PSK-AES). Once
 * librist recovers the TS byte stream it is identical to the MPEG-TS the player
 * already demuxes, so this module exposes the recovered bytes through a
 * basis_read_fn and the caller hands that to basis_ts_run — reusing the whole
 * TS demux -> OS decode -> texture/PCM pipeline unchanged.
 *
 * Built only when BASIS_WITH_RIST is defined (it links librist + mbedTLS, the
 * plugin's first third-party static libs). When not built, basis_rist_open
 * returns NULL and sets a clear "not built" error on the sink, so the dispatch
 * path degrades gracefully instead of failing to link.
 */
#ifndef BASIS_RIST_H
#define BASIS_RIST_H

#include "../basis_media_internal.h"
#include "basis_url.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Open a Main-Profile RIST receiver, connecting as a caller (outbound) to
 * parts->host:parts->port. Encryption/buffer parameters ride in parts->path
 * (the query string: ?secret=…&aes-type=…&buffer=…). `sink` is used only to
 * surface connection errors and to poll is_running while connecting. Returns an
 * opaque context, or NULL on failure (a message is set via the sink). */
void* basis_rist_open(const basis_url_t* parts, basis_media_sink_t* sink);

/* basis_read_fn: copy up to `len` bytes of recovered MPEG-TS into buf. Blocks
 * (with a short internal timeout) until data is available or the receiver is
 * shut down. Returns bytes read (>0), 0 on orderly shutdown, <0 on error. */
int basis_rist_read(void* ctx, uint8_t* buf, int len);

void basis_rist_close(void* ctx);

#ifdef __cplusplus
}
#endif
#endif
