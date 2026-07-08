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

/* reseek/reseek_ctx (optional, NULL when the source can't reposition) let the
 * progressive path honour sink->take_seek requests with a ranged refetch. */
int basis_mp4_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx,
                  basis_reseek_fn reseek, void* reseek_ctx);

#ifdef __cplusplus
}
#endif
#endif
