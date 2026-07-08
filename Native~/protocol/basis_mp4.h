/* MP4 demuxer feeding H.264/H.265 + AAC into a basis_media_sink. Handles both
 * fragmented MP4 (fMP4/CMAF: init segment + moof/mdat fragments) and classic
 * progressive files (moov sample tables + mdat, faststart layout). Pulls bytes
 * through the supplied read callback. */
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
