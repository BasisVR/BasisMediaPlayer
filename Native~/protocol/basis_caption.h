/* In-band CEA-608 closed-caption extraction from the H.264/H.265 video AU path.
 *
 * Captions ride inside the coded video as SEI user-data (ATSC A/53
 * user_data_registered_itu_t_t35, "GA94"), so one scan of each Annex B access
 * unit covers every transport that carries the video transparently (MPEG-TS,
 * RTMP/FLV, RTSP/RTP, RIST, HLS-with-TS). The scanner pulls the cc_data triples,
 * runs the CEA-608 field-1 caption decoder, and stores timed cues keyed to the
 * AU's PTS. A poll returns the cue active at the current presentation position,
 * so cues display in lockstep with the frame they belong to regardless of the
 * decode buffer's lead.
 *
 * Slice 1 decodes CEA-608 field 1 (CC1) to plain text. CEA-708 DTVCC (cc_type
 * 2/3) is parsed out but not yet decoded. */
#ifndef BASIS_CAPTION_H
#define BASIS_CAPTION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct basis_caption_ctx basis_caption_ctx_t;

basis_caption_ctx_t* basis_caption_create(void);
void                 basis_caption_destroy(basis_caption_ctx_t* c);

/* Scan one Annex B video access unit for caption SEI and advance the decoder.
 * hevc selects the H.265 NAL/SEI layout (0 = H.264). Runs on the demux thread;
 * the cue store is internally locked. No-op on a NULL ctx. */
void basis_caption_scan_au(basis_caption_ctx_t* c, const uint8_t* annexb, int len,
                           int hevc, int64_t pts_us);

/* Copy the caption cue active at presentation_pts_us into buf (UTF-8,
 * NUL-terminated). Returns bytes written (0 = no active cue), or -1 on bad args.
 * out_start_us/out_end_us receive the active cue's time range (may be NULL).
 * Safe from any thread. */
int basis_caption_poll(basis_caption_ctx_t* c, int64_t presentation_pts_us,
                       char* buf, int buf_size,
                       int64_t* out_start_us, int64_t* out_end_us);

#ifdef __cplusplus
}
#endif
#endif
