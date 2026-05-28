/* RTMP client (handshake + chunk stream + FLV tag parse) feeding AVC/AAC into a
 * basis_media_sink. Plaintext rtmp only; rtmps (RTMP-over-TLS) is not handled by
 * this module (the platform TLS stacks would be required). */
#ifndef BASIS_RTMP_H
#define BASIS_RTMP_H

#include "../basis_media_internal.h"
#include "basis_url.h"

#ifdef __cplusplus
extern "C" {
#endif

int basis_rtmp_run(basis_media_sink_t* sink, const basis_url_t* url);

#ifdef __cplusplus
}
#endif
#endif
