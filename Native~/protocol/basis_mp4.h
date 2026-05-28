/* Fragmented-MP4 demuxer (ftyp/moov/moof/mdat) feeding H.264/H.265 + AAC into a
 * basis_media_sink. Pulls bytes through the supplied read callback. Targets the
 * live fMP4 profile (init segment + moof/mdat fragments). */
#ifndef BASIS_MP4_H
#define BASIS_MP4_H

#include "../basis_media_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

int basis_mp4_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx);

#ifdef __cplusplus
}
#endif
#endif
