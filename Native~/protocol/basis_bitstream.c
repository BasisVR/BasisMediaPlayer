#include "basis_bitstream.h"

#include <string.h>

static const int kAacRates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000,  7350,  0,     0,     0
};

int basis_aac_sample_rate_index(int sample_rate) {
    for (int i = 0; i < 13; ++i) if (kAacRates[i] == sample_rate) return i;
    return 4; /* 44100 fallback */
}

int basis_aac_sample_rate_from_index(int index) {
    if (index < 0 || index > 15) return 0;
    return kAacRates[index];
}

/* ---- Annex B ------------------------------------------------------------ */

int basis_annexb_next(const uint8_t* data, int size, int from, int* nal_off, int* nal_len) {
    if (!data || from < 0) return -1;
    int i = from;
    /* find start of this NAL: a 00 00 01 (after optional extra 00) */
    int start = -1;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start = i + 3;
            break;
        }
        ++i;
    }
    if (start < 0) return -1;

    /* find next start code -> end of this NAL */
    int j = start;
    int end = size;
    while (j + 3 <= size) {
        if (data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1) {
            end = j;
            /* trim a trailing zero that belongs to a 4-byte start code */
            if (end > start && data[end - 1] == 0) end--;
            break;
        }
        ++j;
    }
    if (nal_off) *nal_off = start;
    if (nal_len) *nal_len = end - start;
    return end;
}

int basis_h264_nal_type(uint8_t b) { return b & 0x1F; }
int basis_h265_nal_type(uint8_t b) { return (b >> 1) & 0x3F; }

int basis_h264_is_keyframe(const uint8_t* annexb, int len) {
    int pos = 0, off, nl;
    while ((pos = basis_annexb_next(annexb, len, pos, &off, &nl)) >= 0) {
        if (nl > 0 && basis_h264_nal_type(annexb[off]) == 5) return 1; /* IDR slice */
    }
    return 0;
}

int basis_h265_is_keyframe(const uint8_t* annexb, int len) {
    int pos = 0, off, nl;
    while ((pos = basis_annexb_next(annexb, len, pos, &off, &nl)) >= 0) {
        if (nl > 0) {
            int t = basis_h265_nal_type(annexb[off]);
            if (t >= 16 && t <= 23) return 1; /* BLA..CRA (IRAP) */
        }
    }
    return 0;
}

/* ---- AVCC -> Annex B ---------------------------------------------------- */

static const uint8_t kStartCode[4] = {0, 0, 0, 1};

int basis_avcc_to_annexb(const uint8_t* in, int in_len, int nls, uint8_t* out, int out_cap) {
    if (!in || nls < 1 || nls > 4) return -1;
    int ip = 0, op = 0;
    while (ip + nls <= in_len) {
        uint32_t nlen = 0;
        for (int k = 0; k < nls; ++k) nlen = (nlen << 8) | in[ip + k];
        ip += nls;
        if (nlen == 0 || ip + (int)nlen > in_len) break;
        if (op + 4 + (int)nlen > out_cap) return -1;
        memcpy(out + op, kStartCode, 4);
        op += 4;
        memcpy(out + op, in + ip, nlen);
        op += (int)nlen;
        ip += (int)nlen;
    }
    return op;
}

/* avcC: [0]=1 [1]=profile [2]=compat [3]=level [4]=0xFC|lengthSizeMinus1
 *       [5]=0xE0|numSPS, then SPS (u16 len + data)..., numPPS, PPS... */
static int avcc_to_annexb(const uint8_t* cfg, int cfg_len, uint8_t* out, int out_cap,
                          int* nal_length_size) {
    if (cfg_len < 7) return -1;
    int op = 0;
    int nls = (cfg[4] & 0x3) + 1;
    if (nal_length_size) *nal_length_size = nls;
    int p = 5;
    int num_sps = cfg[p++] & 0x1F;
    for (int i = 0; i < num_sps && p + 2 <= cfg_len; ++i) {
        int l = (cfg[p] << 8) | cfg[p + 1]; p += 2;
        if (p + l > cfg_len || op + 4 + l > out_cap) return -1;
        memcpy(out + op, kStartCode, 4); op += 4;
        memcpy(out + op, cfg + p, l); op += l; p += l;
    }
    if (p >= cfg_len) return op;
    int num_pps = cfg[p++];
    for (int i = 0; i < num_pps && p + 2 <= cfg_len; ++i) {
        int l = (cfg[p] << 8) | cfg[p + 1]; p += 2;
        if (p + l > cfg_len || op + 4 + l > out_cap) return -1;
        memcpy(out + op, kStartCode, 4); op += 4;
        memcpy(out + op, cfg + p, l); op += l; p += l;
    }
    return op;
}

/* hvcC: 22-byte header, then numOfArrays; each array: [0]=type [1..2]=numNalus,
 * then per-NALU u16 len + data. */
static int hvcc_to_annexb(const uint8_t* cfg, int cfg_len, uint8_t* out, int out_cap,
                          int* nal_length_size) {
    if (cfg_len < 23) return -1;
    int op = 0;
    int nls = (cfg[21] & 0x3) + 1;
    if (nal_length_size) *nal_length_size = nls;
    int num_arrays = cfg[22];
    int p = 23;
    for (int a = 0; a < num_arrays && p + 3 <= cfg_len; ++a) {
        p += 1; /* array_completeness + NAL type */
        int num = (cfg[p] << 8) | cfg[p + 1]; p += 2;
        for (int n = 0; n < num && p + 2 <= cfg_len; ++n) {
            int l = (cfg[p] << 8) | cfg[p + 1]; p += 2;
            if (p + l > cfg_len || op + 4 + l > out_cap) return -1;
            memcpy(out + op, kStartCode, 4); op += 4;
            memcpy(out + op, cfg + p, l); op += l; p += l;
        }
    }
    return op;
}

int basis_avcc_extradata_to_annexb(const uint8_t* cfg, int cfg_len, int hevc,
                                   uint8_t* out, int out_cap, int* nal_length_size) {
    if (!cfg || cfg_len <= 0) return -1;
    return hevc ? hvcc_to_annexb(cfg, cfg_len, out, out_cap, nal_length_size)
                : avcc_to_annexb(cfg, cfg_len, out, out_cap, nal_length_size);
}

/* ---- AAC --------------------------------------------------------------- */

int basis_adts_parse(const uint8_t* p, int len, basis_adts_t* out) {
    if (!p || len < 7 || !out) return -1;
    if (p[0] != 0xFF || (p[1] & 0xF0) != 0xF0) return -1; /* syncword */
    int protection_absent = p[1] & 0x01;
    int profile = ((p[2] >> 6) & 0x3);             /* 0..3 (AOT-1) */
    int sr_index = (p[2] >> 2) & 0x0F;
    int channels = ((p[2] & 0x1) << 2) | ((p[3] >> 6) & 0x3);
    int frame_len = ((p[3] & 0x3) << 11) | (p[4] << 3) | ((p[5] >> 5) & 0x7);
    out->profile = profile;
    out->sample_rate = basis_aac_sample_rate_from_index(sr_index);
    out->channels = channels;
    out->frame_len = frame_len;
    out->header_len = protection_absent ? 7 : 9;
    if (frame_len < out->header_len) return -1;
    return 0;
}

int basis_aac_build_asc(int object_type, int sample_rate, int channels, uint8_t out[2]) {
    int sri = basis_aac_sample_rate_index(sample_rate);
    /* 5 bits AOT, 4 bits sample-rate index, 4 bits channel config */
    out[0] = (uint8_t)((object_type << 3) | ((sri >> 1) & 0x7));
    out[1] = (uint8_t)(((sri & 0x1) << 7) | ((channels & 0xF) << 3));
    return 2;
}

int basis_aac_build_adts(uint8_t out[7], int object_type, int sample_rate,
                         int channels, int aac_len) {
    int sri = basis_aac_sample_rate_index(sample_rate);
    int profile = object_type - 1; /* ADTS profile = AOT - 1 */
    int frame_len = aac_len + 7;
    out[0] = 0xFF;
    out[1] = 0xF1; /* MPEG-4, no CRC */
    out[2] = (uint8_t)(((profile & 0x3) << 6) | ((sri & 0xF) << 2) | ((channels >> 2) & 0x1));
    out[3] = (uint8_t)(((channels & 0x3) << 6) | ((frame_len >> 11) & 0x3));
    out[4] = (uint8_t)((frame_len >> 3) & 0xFF);
    out[5] = (uint8_t)(((frame_len & 0x7) << 5) | 0x1F);
    out[6] = 0xFC;
    return 7;
}

/* ---- H.264 SPS dimensions (exp-golomb) --------------------------------- */

typedef struct { const uint8_t* d; int size; int bitpos; } gb_t;

static uint32_t gb_u(gb_t* g, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        int byte = g->bitpos >> 3;
        int bit = 7 - (g->bitpos & 7);
        int b = (byte < g->size) ? ((g->d[byte] >> bit) & 1) : 0;
        v = (v << 1) | (uint32_t)b;
        g->bitpos++;
    }
    return v;
}
static uint32_t gb_ue(gb_t* g) {
    int zeros = 0;
    while (gb_u(g, 1) == 0 && zeros < 32) zeros++;
    uint32_t v = (1u << zeros) - 1 + gb_u(g, zeros);
    return v;
}
static int32_t gb_se(gb_t* g) {
    uint32_t ue = gb_ue(g);
    return (ue & 1) ? (int32_t)((ue + 1) >> 1) : -(int32_t)(ue >> 1);
}

int basis_h264_sps_dimensions(const uint8_t* sps, int len, int* width, int* height) {
    if (!sps || len < 4) return -1;
    /* strip NAL header byte; remove emulation-prevention 0x03 */
    uint8_t rbsp[512];
    int rlen = 0, zeros = 0;
    for (int i = 1; i < len && rlen < (int)sizeof(rbsp); ++i) {
        if (zeros >= 2 && sps[i] == 0x03) { zeros = 0; continue; }
        rbsp[rlen++] = sps[i];
        zeros = (sps[i] == 0) ? zeros + 1 : 0;
    }
    gb_t g = { rbsp, rlen, 0 };
    uint32_t profile_idc = gb_u(&g, 8);
    gb_u(&g, 8);            /* constraint flags + reserved */
    gb_u(&g, 8);            /* level_idc */
    gb_ue(&g);             /* seq_parameter_set_id */
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
        uint32_t chroma = gb_ue(&g);
        if (chroma == 3) gb_u(&g, 1);
        gb_ue(&g); gb_ue(&g);
        gb_u(&g, 1);
        if (gb_u(&g, 1)) { /* scaling matrix — skip (uncommon in SPS) */ }
    }
    gb_ue(&g);                          /* log2_max_frame_num */
    uint32_t poc_type = gb_ue(&g);
    if (poc_type == 0) {
        gb_ue(&g);
    } else if (poc_type == 1) {
        gb_u(&g, 1);
        gb_se(&g); gb_se(&g);
        uint32_t n = gb_ue(&g);
        for (uint32_t i = 0; i < n; ++i) gb_se(&g);
    }
    gb_ue(&g);                          /* max_num_ref_frames */
    gb_u(&g, 1);                        /* gaps_in_frame_num */
    uint32_t w_mbs = gb_ue(&g) + 1;
    uint32_t h_map = gb_ue(&g) + 1;
    uint32_t frame_mbs_only = gb_u(&g, 1);
    if (!frame_mbs_only) gb_u(&g, 1);
    gb_u(&g, 1);                        /* direct_8x8 */
    uint32_t crop = gb_u(&g, 1);
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (crop) { cl = gb_ue(&g); cr = gb_ue(&g); ct = gb_ue(&g); cb = gb_ue(&g); }

    int w = (int)(w_mbs * 16) - (int)(cl + cr) * 2;
    int h = (int)(h_map * 16 * (2 - frame_mbs_only)) - (int)(ct + cb) * 2;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    if (width) *width = w;
    if (height) *height = h;
    return 0;
}
