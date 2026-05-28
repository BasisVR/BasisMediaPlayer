/*
 * basis_mp4.c — fragmented-MP4 demuxer for live streams (init segment then a
 * sequence of moof+mdat fragments) -> H.264/H.265 + AAC.
 *
 * Parses moov (track config: codec, timescale, avcC/hvcC -> Annex B extradata,
 * esds -> AAC ASC) and each moof (tfhd/tfdt/trun: per-sample sizes, durations,
 * composition offsets, base decode time), then slices the following mdat into
 * samples in trun order.
 *
 * Common-case assumptions (sufficient for VRCDN-style fMP4, flagged for iteration):
 *   - one video track + one audio track
 *   - one trun per traf, mdat samples contiguous in trun order
 *   - 4-byte NAL length (from avcC); version 0/1 boxes
 */

#include "basis_mp4.h"
#include "basis_bitstream.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int track_id;
    int is_video;
    basis_codec_t codec;
    int timescale;
    uint8_t extradata[2048];
    int extradata_len;
    int nal_len_size;
    uint8_t asc[16];
    int asc_len;
    int sr, ch, obj;
    int64_t next_dts; /* running, in track timescale */
    int announced;
} mp4_track_t;

typedef struct {
    basis_media_sink_t* sink;
    basis_read_fn read;
    void* ctx;
    mp4_track_t tracks[2];
    int ntracks;

    /* pending fragment (from the last moof) */
    int frag_track_id;
    int64_t frag_base_dts;
    int frag_default_dur;
    int frag_default_size;
    uint32_t* sizes;
    uint32_t* durs;
    int32_t*  ctos;
    int frag_count;
    int frag_cap;
} mp4_t;

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p)<<32)|rd32(p+4); }

static int read_exact(mp4_t* m, uint8_t* buf, int n) {
    int got = 0;
    while (got < n) {
        if (!m->sink->is_running(m->sink->user)) return got;
        int r = m->read(m->ctx, buf + got, n - got);
        if (r <= 0) return got;
        got += r;
    }
    return got;
}

/* reads a top-level box: header + body into *out (caller frees). type is FOURCC. */
static int read_box(mp4_t* m, uint32_t* type, uint8_t** out, int64_t* out_len) {
    uint8_t hdr[8];
    if (read_exact(m, hdr, 8) != 8) return -1;
    int64_t size = rd32(hdr);
    *type = rd32(hdr + 4);
    int header = 8;
    if (size == 1) {
        uint8_t ext[8];
        if (read_exact(m, ext, 8) != 8) return -1;
        size = (int64_t)rd64(ext);
        header = 16;
    }
    if (size < header || size > 256LL * 1024 * 1024) return -1;
    int64_t body = size - header;
    uint8_t* buf = (uint8_t*)malloc((size_t)body ? (size_t)body : 1);
    if (!buf) return -1;
    if (read_exact(m, buf, (int)body) != (int)body) { free(buf); return -1; }
    *out = buf; *out_len = body;
    return 0;
}

static mp4_track_t* track_by_id(mp4_t* m, int id) {
    for (int i = 0; i < m->ntracks; ++i) if (m->tracks[i].track_id == id) return &m->tracks[i];
    return NULL;
}

/* ---- moov parsing ------------------------------------------------------- */

static void parse_stsd(mp4_track_t* t, const uint8_t* p, int len) {
    /* stsd: version/flags(4) entry_count(4) then sample entries */
    if (len < 8) return;
    int n = (int)rd32(p + 4);
    int off = 8;
    for (int e = 0; e < n && off + 8 <= len; ++e) {
        int esize = (int)rd32(p + off);
        uint32_t etype = rd32(p + off + 4);
        const uint8_t* ent = p + off;
        if (esize < 8 || off + esize > len) break;

        if (etype == 0x61766331 /*avc1*/ || etype == 0x68766331 /*hvc1*/ || etype == 0x68657631 /*hev1*/) {
            t->is_video = 1;
            t->codec = (etype == 0x61766331) ? BASIS_CODEC_H264 : BASIS_CODEC_H265;
            /* visual sample entry header is 78 bytes, then child boxes (avcC/hvcC) */
            int co = 8 + 78;
            while (co + 8 <= esize) {
                int csz = (int)rd32(ent + co);
                uint32_t ct = rd32(ent + co + 4);
                if (csz < 8 || co + csz > esize) break;
                if (ct == 0x61766343 /*avcC*/ || ct == 0x68766343 /*hvcC*/) {
                    int nls = 4;
                    int got = basis_avcc_extradata_to_annexb(ent + co + 8, csz - 8,
                                t->codec == BASIS_CODEC_H265, t->extradata, sizeof(t->extradata), &nls);
                    if (got > 0) { t->extradata_len = got; t->nal_len_size = nls; }
                }
                co += csz;
            }
        } else if (etype == 0x6d703461 /*mp4a*/) {
            t->is_video = 0;
            t->codec = BASIS_CODEC_AAC;
            /* audio sample entry header 28 bytes, then esds */
            t->ch = (ent[8 + 16] << 8) | ent[8 + 17];
            t->sr = (int)(rd32(ent + 8 + 24) >> 16);
            int co = 8 + 28;
            while (co + 8 <= esize) {
                int csz = (int)rd32(ent + co);
                uint32_t ct = rd32(ent + co + 4);
                if (csz < 8 || co + csz > esize) break;
                if (ct == 0x65736473 /*esds*/) {
                    /* find DecoderSpecificInfo (tag 0x05) inside esds */
                    const uint8_t* ep = ent + co + 12; int el = csz - 12;
                    for (int i = 0; i + 2 < el; ++i) {
                        if (ep[i] == 0x05) {
                            int j = i + 1, dlen = 0, b;
                            do { b = ep[j++]; dlen = (dlen << 7) | (b & 0x7F); } while ((b & 0x80) && j < el);
                            if (j + dlen <= el && dlen <= (int)sizeof(t->asc)) {
                                memcpy(t->asc, ep + j, dlen); t->asc_len = dlen;
                                int aot = (t->asc[0] >> 3) & 0x1F;
                                int sri = ((t->asc[0] & 7) << 1) | (t->asc[1] >> 7);
                                t->obj = aot;
                                if (!t->sr) t->sr = basis_aac_sample_rate_from_index(sri);
                                if (!t->ch) t->ch = (t->asc[1] >> 3) & 0xF;
                            }
                            break;
                        }
                    }
                }
                co += csz;
            }
        }
        off += esize;
    }
}

static void parse_box_tree(mp4_t* m, mp4_track_t* t, const uint8_t* p, int len);

static void parse_trak(mp4_t* m, const uint8_t* p, int len) {
    if (m->ntracks >= 2) return;
    mp4_track_t* t = &m->tracks[m->ntracks];
    memset(t, 0, sizeof(*t));
    t->nal_len_size = 4;
    t->timescale = 90000;
    parse_box_tree(m, t, p, len);
    if (t->codec != BASIS_CODEC_NONE) m->ntracks++;
}

static void parse_box_tree(mp4_t* m, mp4_track_t* t, const uint8_t* p, int len) {
    int off = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || off + sz > len) break;
        const uint8_t* body = p + off + 8;
        int blen = sz - 8;
        switch (ty) {
            case 0x7472616b: parse_trak(m, body, blen); break;          /* trak */
            case 0x6d646961: /* mdia */
            case 0x6d696e66: /* minf */
            case 0x7374626c: parse_box_tree(m, t, body, blen); break;    /* stbl */
            case 0x6d646864: /* mdhd: version(1) flags(3) ... timescale */
                if (t) { int ver = body[0]; t->track_id = t->track_id; int tsoff = ver == 1 ? 4 + 8 + 8 : 4 + 4 + 4; if (tsoff + 4 <= blen) t->timescale = (int)rd32(body + tsoff); }
                break;
            case 0x746b6864: /* tkhd: track id */
                if (t) { int ver = body[0]; int idoff = ver == 1 ? 4 + 8 + 8 : 4 + 4 + 4; if (idoff + 4 <= blen) t->track_id = (int)rd32(body + idoff); }
                break;
            case 0x73747364: if (t) parse_stsd(t, body, blen); break;    /* stsd */
            default: break;
        }
        off += sz;
    }
}

static void announce_tracks(mp4_t* m) {
    for (int i = 0; i < m->ntracks; ++i) {
        mp4_track_t* t = &m->tracks[i];
        if (t->announced) continue;
        if (t->is_video) {
            int w = 0, h = 0;
            if (t->codec == BASIS_CODEC_H264 && t->extradata_len) {
                int pos=0,no,nl;
                while ((pos=basis_annexb_next(t->extradata,t->extradata_len,pos,&no,&nl))>=0)
                    if (nl>0 && basis_h264_nal_type(t->extradata[no])==7){ basis_h264_sps_dimensions(t->extradata+no,nl,&w,&h); break; }
            }
            m->sink->on_video_format(m->sink->user, t->codec, t->extradata, t->extradata_len, w, h);
        } else {
            m->sink->on_audio_format(m->sink->user, BASIS_CODEC_AAC, t->sr ? t->sr : 48000, t->ch ? t->ch : 2,
                                     t->asc_len ? t->asc : NULL, t->asc_len);
        }
        t->announced = 1;
    }
}

/* ---- moof parsing ------------------------------------------------------- */

static int frag_reserve(mp4_t* m, int n) {
    if (n <= m->frag_cap) return 1;
    int nc = m->frag_cap ? m->frag_cap * 2 : 256;
    while (nc < n) nc *= 2;
    uint32_t* s = (uint32_t*)realloc(m->sizes, (size_t)nc * sizeof(uint32_t));
    uint32_t* d = (uint32_t*)realloc(m->durs, (size_t)nc * sizeof(uint32_t));
    int32_t*  c = (int32_t*)realloc(m->ctos, (size_t)nc * sizeof(int32_t));
    if (!s || !d || !c) { free(s); free(d); free(c); return 0; }
    m->sizes = s; m->durs = d; m->ctos = c; m->frag_cap = nc;
    return 1;
}

static void parse_traf(mp4_t* m, const uint8_t* p, int len) {
    int off = 0;
    int track_id = 0, default_dur = 0, default_size = 0;
    int64_t base_dts = 0;
    m->frag_count = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || off + sz > len) break;
        const uint8_t* b = p + off + 8;
        int bl = sz - 8;
        if (ty == 0x74666864) { /* tfhd */
            uint32_t flags = rd32(b) & 0xFFFFFF;
            int q = 4;
            track_id = (int)rd32(b + q); q += 4;
            if (flags & 0x000001) q += 8;  /* base-data-offset */
            if (flags & 0x000002) q += 4;  /* sample-description-index */
            if (flags & 0x000008) { default_dur = (int)rd32(b + q); q += 4; }
            if (flags & 0x000010) { default_size = (int)rd32(b + q); q += 4; }
        } else if (ty == 0x74666474) { /* tfdt */
            int ver = b[0];
            base_dts = ver == 1 ? (int64_t)rd64(b + 4) : (int64_t)rd32(b + 4);
        } else if (ty == 0x7472756e) { /* trun */
            uint32_t flags = rd32(b) & 0xFFFFFF;
            int count = (int)rd32(b + 4);
            int q = 8;
            if (flags & 0x000001) q += 4; /* data-offset */
            if (flags & 0x000004) q += 4; /* first-sample-flags */
            if (!frag_reserve(m, count)) { count = 0; }
            for (int i = 0; i < count; ++i) {
                uint32_t dur = (uint32_t)default_dur, size = (uint32_t)default_size;
                int32_t cto = 0;
                if (flags & 0x000100) { dur = rd32(b + q); q += 4; }
                if (flags & 0x000200) { size = rd32(b + q); q += 4; }
                if (flags & 0x000400) { q += 4; }            /* sample flags */
                if (flags & 0x000800) { cto = (int32_t)rd32(b + q); q += 4; }
                m->sizes[i] = size; m->durs[i] = dur; m->ctos[i] = cto;
            }
            m->frag_count = count;
        }
        off += sz;
    }
    m->frag_track_id = track_id;
    m->frag_base_dts = base_dts;
    m->frag_default_dur = default_dur;
    m->frag_default_size = default_size;
}

static void parse_moof(mp4_t* m, const uint8_t* p, int len) {
    int off = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || off + sz > len) break;
        if (ty == 0x74726166) parse_traf(m, p + off + 8, sz - 8); /* traf */
        off += sz;
    }
}

static void consume_mdat(mp4_t* m, const uint8_t* data, int len) {
    mp4_track_t* t = track_by_id(m, m->frag_track_id);
    if (!t || m->frag_count <= 0) return;

    int64_t dts = m->frag_base_dts;
    int pos = 0;
    int ts = t->timescale > 0 ? t->timescale : 90000;

    for (int i = 0; i < m->frag_count; ++i) {
        int ssize = (int)m->sizes[i];
        if (ssize <= 0 || pos + ssize > len) break;
        int64_t pts_units = dts + m->ctos[i];
        int64_t pts_us = pts_units * 1000000 / ts;

        if (t->is_video) {
            uint8_t* out = (uint8_t*)malloc((size_t)ssize + 64);
            if (out) {
                int n = basis_avcc_to_annexb(data + pos, ssize, t->nal_len_size ? t->nal_len_size : 4, out, ssize + 64);
                if (n > 0) {
                    int key = t->codec == BASIS_CODEC_H265 ? basis_h265_is_keyframe(out, n) : basis_h264_is_keyframe(out, n);
                    m->sink->on_video_au(m->sink->user, out, n, pts_us, key);
                }
                free(out);
            }
        } else {
            m->sink->on_audio_frame(m->sink->user, data + pos, ssize, pts_us);
        }

        pos += ssize;
        dts += m->durs[i] ? m->durs[i] : m->frag_default_dur;
    }
}

int basis_mp4_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx) {
    mp4_t m; memset(&m, 0, sizeof(m));
    m.sink = sink; m.read = read; m.ctx = ctx;

    while (sink->is_running(sink->user)) {
        uint32_t type; uint8_t* buf; int64_t blen;
        if (read_box(&m, &type, &buf, &blen) != 0) break;

        switch (type) {
            case 0x6d6f6f76: /* moov */
                parse_box_tree(&m, NULL, buf, (int)blen);
                announce_tracks(&m);
                break;
            case 0x6d6f6f66: /* moof */
                parse_moof(&m, buf, (int)blen);
                break;
            case 0x6d646174: /* mdat */
                consume_mdat(&m, buf, (int)blen);
                break;
            default:
                break; /* ftyp, styp, sidx, free, ... ignored */
        }
        free(buf);
    }

    free(m.sizes); free(m.durs); free(m.ctos);
    return 0;
}
