/* H.264/H.265 NAL helpers + AAC ADTS/ASC helpers shared by the demuxers. */
#ifndef BASIS_BITSTREAM_H
#define BASIS_BITSTREAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Annex B (start-code) NAL iteration --------------------------------- */

/* Finds the next NAL unit (between 00 00 01 / 00 00 00 01 start codes) at or
 * after `from`. On success sets *nal_off/*nal_len (payload excluding the start
 * code) and returns the index just past this NAL; returns -1 when none remain. */
int basis_annexb_next(const uint8_t* data, int size, int from, int* nal_off, int* nal_len);

/* H.264 NAL type = byte & 0x1F ; H.265 NAL type = (byte >> 1) & 0x3F */
int basis_h264_nal_type(uint8_t first_byte);
int basis_h265_nal_type(uint8_t first_byte);

/* 1 if the access unit contains an IDR/keyframe NAL. */
int basis_h264_is_keyframe(const uint8_t* annexb, int len);
int basis_h265_is_keyframe(const uint8_t* annexb, int len);

/* ---- AVCC (length-prefixed) -> Annex B ---------------------------------- */

/* Converts a length-prefixed sample (e.g. an MP4 sample or FLV AVC NALU) to
 * Annex B, prepending a 4-byte start code to each NALU. Returns bytes written to
 * `out`, or -1 if `out_cap` is too small. nal_length_size is 1..4 (usually 4). */
int basis_avcc_to_annexb(const uint8_t* in, int in_len, int nal_length_size,
                         uint8_t* out, int out_cap);

/* Builds an Annex B extradata blob from an avcC/hvcC config record (as carried
 * in MP4 stsd or FLV AVCDecoderConfigurationRecord). Writes the contained
 * SPS/PPS (and VPS for HEVC) as start-code-prefixed NALUs. Returns bytes written
 * or -1. Sets *nal_length_size to the record's length size. */
int basis_avcc_extradata_to_annexb(const uint8_t* cfg, int cfg_len, int hevc,
                                   uint8_t* out, int out_cap, int* nal_length_size);

/* ---- AAC ---------------------------------------------------------------- */

typedef struct basis_adts {
    int sample_rate;
    int channels;
    int profile;      /* MPEG-4 audio object type - 1 (ADTS profile field) */
    int frame_len;    /* full frame length incl. header */
    int header_len;   /* 7 or 9 */
} basis_adts_t;

/* Parses an ADTS header at `p`. Returns 0 on success. */
int basis_adts_parse(const uint8_t* p, int len, basis_adts_t* out);

/* Builds a 2-byte AudioSpecificConfig from object type (=profile+1), sample rate
 * and channel count. Returns 2. */
int basis_aac_build_asc(int object_type, int sample_rate, int channels, uint8_t out[2]);

/* Builds a 7-byte ADTS header for a raw AAC frame of `aac_len` bytes. Returns 7. */
int basis_aac_build_adts(uint8_t out[7], int object_type, int sample_rate,
                         int channels, int aac_len);

int basis_aac_sample_rate_index(int sample_rate);
int basis_aac_sample_rate_from_index(int index);

/* ---- H.264 SPS dimensions (best-effort) --------------------------------- */

/* Parses width/height from an H.264 SPS NAL (payload without start code).
 * Returns 0 on success. Useful to report size before the decoder produces a
 * frame; the decoder remains the source of truth. */
int basis_h264_sps_dimensions(const uint8_t* sps, int len, int* width, int* height);

#ifdef __cplusplus
}
#endif
#endif
