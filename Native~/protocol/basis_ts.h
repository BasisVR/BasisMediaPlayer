/* MPEG-TS demuxer (PAT/PMT/PES) feeding H.264/H.265 + AAC into a basis_media_sink.
 * Pulls bytes through the supplied read callback (TCP or HTTP byte source). */
#ifndef BASIS_TS_H
#define BASIS_TS_H

#include "../basis_media_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int basis_ts_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx);

#ifdef __cplusplus
}
#endif
#endif
