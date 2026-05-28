/* RTSP client with RTP interleaved over the TCP control channel (rtspt) feeding
 * H.264/H.265 access units into a basis_media_sink. Blocks on the demux thread
 * until the sink stops running or the connection fails. Returns 0 on clean EOF. */
#ifndef BASIS_RTSP_H
#define BASIS_RTSP_H

#include "../basis_media_internal.h"
#include "basis_url.h"

#ifdef __cplusplus
extern "C" {
#endif

int basis_rtsp_run(basis_media_sink_t* sink, const basis_url_t* url);

#ifdef __cplusplus
}
#endif
#endif
