/*
 * basis_mp4.c — MP4 demuxer -> H.264/H.265 + AAC. Handles both MP4 shapes:
 *
 *   fragmented (fMP4/CMAF): init segment (moov) then moof+mdat fragments —
 *     live streams, DASH/adaptive legs, HLS fMP4 segments. Each moof's
 *     tfhd/tfdt/trun tables slice the following mdat.
 *
 *   progressive (classic): one moov carrying the full sample tables
 *     (stts/ctts/stsc/stco/stsz/stss) and one big mdat — plain recorded .mp4
 *     files. Requires faststart layout (moov before mdat): the byte source
 *     only moves forward, so a trailing moov can't index media that has
 *     already streamed past — that shape gets a clear error instead.
 *
 * Both shapes parse moov for track config (codec, timescale, avcC/hvcC ->
 * Annex B extradata, esds -> AAC ASC) and emit tracks interleaved in decode
 * order (fragments) or the muxer's file order (progressive).
 *
 * Common-case assumptions (sufficient for VRCDN-style fMP4 and web-optimised
 * progressive files, flagged for iteration):
 *   - one video track + one audio track
 *   - one trun per traf, mdat samples contiguous in trun order
 *   - version 0/1 boxes; stsz sample sizes (stz2 raises a clear error)
 */

#include "basis_mp4.h"
#include "basis_bitstream.h"

#include <stdlib.h>
#include <string.h>

#define MP4_MAX_FRAGS 4   /* per-moof trun runs (audio + video, with headroom) */
#define MP4_MAX_BOX   (256LL * 1024 * 1024)  /* cap for boxes buffered whole (moov/moof/fragment mdat) */
#define MP4_MAX_TABLE (16u * 1024 * 1024)    /* per-table entry cap (~hours of samples) */
#define MP4_MAX_FRAG_SAMPLES (1u << 20)      /* per-trun sample cap (a fragment is seconds of media) */

/* Classic (progressive) sample tables + walk cursor. Valid when sample_count
 * and chunk_count are non-zero; fMP4 init segments carry these boxes with zero
 * entries, which leaves the whole struct inert. */
typedef struct {
    uint32_t  sample_count;
    uint32_t  const_size;     /* stsz sample_size != 0 => every sample this size */
    uint32_t* sizes;          /* per-sample sizes when const_size == 0 */
    uint32_t  stts_count;     /* {sample_count, delta} run pairs */
    uint32_t* stts;
    uint32_t  ctts_count;     /* {sample_count, offset} run pairs; sign depends on ctts_version */
    uint32_t* ctts;
    int       ctts_version;   /* FullBox version: 0 = unsigned offsets, 1 = signed */
    int       has_stz2;       /* compact sample sizes present (unsupported) */
    uint32_t  stsc_count;     /* {first_chunk, samples_per_chunk} pairs */
    uint32_t* stsc;
    uint32_t  chunk_count;
    uint64_t* chunk_offsets;  /* absolute file offsets (stco or co64) */
    uint32_t  stss_count;     /* sync-sample numbers, 1-based; NULL => all sync */
    uint32_t* stss;

    /* edit-list mapping onto the movie timeline (from elst, resolved once the
     * movie timescale is known): presentation = media time - media_start,
     * shifted late by pts_delay_us of initial empty edits. */
    int64_t   media_start;    /* media-timescale ticks */
    int64_t   pts_delay_us;

    /* cursor: the next sample to emit */
    uint32_t next;            /* global sample index, 0-based */
    uint32_t chunk;           /* current chunk, 0-based */
    uint32_t chunk_sample;    /* sample index within the current chunk */
    uint64_t offset;          /* absolute file offset of the next sample */
    uint32_t stsc_i;
    uint32_t stts_i, stts_used;
    uint32_t ctts_i, ctts_used;
    uint32_t stss_i;
    int64_t  dts;             /* running, in track timescale */
} mp4_ctab_t;

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
    int announced;
    /* raw elst entries; durations are movie-timescale, media times media-timescale */
    uint32_t elst_count;
    struct { uint64_t duration; int64_t media_time; } elst[8];
    mp4_ctab_t ctab;
} mp4_track_t;

/* One trun run within a moof: its track, where its samples sit (data-offset,
 * relative to the moof start), and the per-sample table. A moof carries one of
 * these per track (audio + video), all slicing the single following mdat. */
typedef struct {
    int       track_id;
    int       data_offset;
    int64_t   base_dts;
    uint32_t  default_dur;
    uint32_t* sizes;
    uint32_t* durs;
    int32_t*  ctos;
    int       count;
    int       cap;
} mp4_frag_t;

typedef struct {
    basis_media_sink_t* sink;
    basis_read_fn read;
    void* ctx;
    mp4_track_t tracks[2];
    int ntracks;
    int movie_timescale;    /* from mvhd; units of elst segment durations */
    int64_t movie_duration; /* from mvhd, movie-timescale ticks; 0 when absent */

    int64_t pos;          /* absolute stream offset consumed so far */
    int     mdat_skipped; /* media data streamed past before any index arrived */

    /* runs from the last moof (one per traf/trun), consumed against its mdat */
    mp4_frag_t frags[MP4_MAX_FRAGS];
    int nfrags;
} mp4_t;

static uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t rd64(const uint8_t* p) { return ((uint64_t)rd32(p)<<32)|rd32(p+4); }

/* Timescale ticks -> microseconds. Splitting into whole seconds + remainder
 * keeps the intermediate multiply in range for any stream-supplied timescale
 * (v * 1000000 directly overflows past ~2.5 hours at a crafted ~1 GHz scale). */
static int64_t ticks_to_us(int64_t v, int timescale) {
    int64_t ts = timescale > 0 ? timescale : 90000;
    int64_t s = v / ts, r = v % ts;
    return s * 1000000 + r * 1000000 / ts;
}

static int read_exact(mp4_t* m, uint8_t* buf, int n) {
    int got = 0;
    while (got < n) {
        if (!m->sink->is_running(m->sink->user)) break;
        int r = m->read(m->ctx, buf + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    m->pos += got;
    return got;
}

static int skip_bytes(mp4_t* m, int64_t n) {
    uint8_t tmp[16384];
    while (n > 0) {
        if (!m->sink->is_running(m->sink->user)) return -1;
        int want = n > (int64_t)sizeof(tmp) ? (int)sizeof(tmp) : (int)n;
        int r = m->read(m->ctx, tmp, want);
        if (r <= 0) return -1;
        m->pos += r;
        n -= r;
    }
    return 0;
}

/* Reads a top-level box header. *body_len is the payload length, or -1 for a
 * size-0 box (extends to end of stream). The caller decides whether to buffer,
 * stream, or skip the payload. */
static int read_box_header(mp4_t* m, uint32_t* type, int64_t* body_len) {
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
    if (size == 0) { *body_len = -1; return 0; }
    if (size < header) return -1;
    *body_len = size - header;
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
        if (esize < 8 || esize > len - off) break;

        if (etype == 0x61766331 /*avc1*/ || etype == 0x68766331 /*hvc1*/ || etype == 0x68657631 /*hev1*/) {
            t->is_video = 1;
            t->codec = (etype == 0x61766331) ? BASIS_CODEC_H264 : BASIS_CODEC_H265;
            /* visual sample entry header is 78 bytes, then child boxes (avcC/hvcC) */
            int co = 8 + 78;
            while (co + 8 <= esize) {
                int csz = (int)rd32(ent + co);
                uint32_t ct = rd32(ent + co + 4);
                if (csz < 8 || csz > esize - co) break;
                if (ct == 0x61766343 /*avcC*/ || ct == 0x68766343 /*hvcC*/) {
                    int nls = 4;
                    int got = basis_avcc_extradata_to_annexb(ent + co + 8, csz - 8,
                                t->codec == BASIS_CODEC_H265, t->extradata, sizeof(t->extradata), &nls);
                    if (got > 0) { t->extradata_len = got; t->nal_len_size = nls; }
                }
                co += csz;
            }
        } else if (etype == 0x6d703461 /*mp4a*/ && esize >= 8 + 28) {
            t->is_video = 0;
            t->codec = BASIS_CODEC_AAC;
            /* audio sample entry header 28 bytes, then esds */
            t->ch = (ent[8 + 16] << 8) | ent[8 + 17];
            t->sr = (int)(rd32(ent + 8 + 24) >> 16);
            int co = 8 + 28;
            while (co + 8 <= esize) {
                int csz = (int)rd32(ent + co);
                uint32_t ct = rd32(ent + co + 4);
                if (csz < 8 || csz > esize - co) break;
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
                                if (!t->ch) t->ch = basis_aac_channels_from_config((t->asc[1] >> 3) & 0xF);
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

/* Reads a fixed-stride sample-table box into a u32 array, keeping `keep` of
 * the `stride` u32 fields per entry. Returns NULL (count 0) on any bound or
 * allocation failure — an incomplete table just leaves the file unplayable as
 * progressive rather than risking a bad walk. */
static uint32_t* parse_table(const uint8_t* p, int len, int stride, int keep, uint32_t* out_count) {
    *out_count = 0;
    if (len < 8) return NULL;
    uint32_t n = rd32(p + 4);
    if (!n || n > MP4_MAX_TABLE) return NULL;
    if ((int64_t)8 + (int64_t)n * stride * 4 > len) return NULL;
    uint32_t* v = (uint32_t*)malloc((size_t)n * keep * sizeof(uint32_t));
    if (!v) return NULL;
    for (uint32_t i = 0; i < n; ++i)
        for (int k = 0; k < keep; ++k)
            v[i * keep + k] = rd32(p + 8 + (int64_t)i * stride * 4 + k * 4);
    *out_count = n;
    return v;
}

static void parse_stsz(mp4_ctab_t* c, const uint8_t* p, int len) {
    if (len < 12) return;
    uint32_t cs = rd32(p + 4);
    uint32_t n = rd32(p + 8);
    if (!n || n > MP4_MAX_TABLE) return;
    if (cs) { c->const_size = cs; c->sample_count = n; return; }
    if ((int64_t)12 + (int64_t)n * 4 > len) return;
    c->sizes = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!c->sizes) return;
    for (uint32_t i = 0; i < n; ++i) c->sizes[i] = rd32(p + 12 + (int64_t)i * 4);
    c->sample_count = n;
}

static void parse_elst(mp4_track_t* t, const uint8_t* p, int len) {
    if (t->elst_count || len < 8) return;
    int ver = p[0];
    uint32_t n = rd32(p + 4);
    int stride = ver == 1 ? 20 : 12;
    if (!n || (int64_t)8 + (int64_t)n * stride > len) return;
    uint32_t keep = n < 8 ? n : 8;  /* only the leading edits shape the walk */
    for (uint32_t i = 0; i < keep; ++i) {
        const uint8_t* e = p + 8 + (size_t)i * stride;
        if (ver == 1) { t->elst[i].duration = rd64(e); t->elst[i].media_time = (int64_t)rd64(e + 8); }
        else          { t->elst[i].duration = rd32(e); t->elst[i].media_time = (int32_t)rd32(e + 4); }
    }
    t->elst_count = keep;
}

static void parse_chunk_offsets(mp4_ctab_t* c, const uint8_t* p, int len, int is64) {
    if (len < 8) return;
    uint32_t n = rd32(p + 4);
    int stride = is64 ? 8 : 4;
    if (!n || n > MP4_MAX_TABLE) return;
    if ((int64_t)8 + (int64_t)n * stride > len) return;
    c->chunk_offsets = (uint64_t*)malloc((size_t)n * sizeof(uint64_t));
    if (!c->chunk_offsets) return;
    for (uint32_t i = 0; i < n; ++i)
        c->chunk_offsets[i] = is64 ? rd64(p + 8 + (int64_t)i * 8) : (uint64_t)rd32(p + 8 + (int64_t)i * 4);
    c->chunk_count = n;
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
    else {
        free(t->ctab.sizes); free(t->ctab.stts); free(t->ctab.ctts);
        free(t->ctab.stsc); free(t->ctab.chunk_offsets); free(t->ctab.stss);
        memset(&t->ctab, 0, sizeof(t->ctab));
    }
}

static void parse_box_tree(mp4_t* m, mp4_track_t* t, const uint8_t* p, int len) {
    int off = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || sz > len - off) break;
        const uint8_t* body = p + off + 8;
        int blen = sz - 8;
        switch (ty) {
            case 0x7472616b: parse_trak(m, body, blen); break;          /* trak */
            case 0x6d646961: /* mdia */
            case 0x6d696e66: /* minf */
            case 0x65647473: /* edts */
            case 0x7374626c: parse_box_tree(m, t, body, blen); break;    /* stbl */
            case 0x6d766864: /* mvhd: movie timescale (elst duration units) + duration */
                if (blen >= 4) {
                    int ver = body[0];
                    int tsoff = ver == 1 ? 4 + 8 + 8 : 4 + 4 + 4;
                    if (tsoff + 4 <= blen) m->movie_timescale = (int)rd32(body + tsoff);
                    if (ver == 1) { if (tsoff + 4 + 8 <= blen) m->movie_duration = (int64_t)rd64(body + tsoff + 4); }
                    else          { uint32_t d = tsoff + 4 + 4 <= blen ? rd32(body + tsoff + 4) : 0;
                                    if (d != 0xFFFFFFFFu) m->movie_duration = d; } /* all-ones = unknown */
                }
                break;
            case 0x656c7374: if (t) parse_elst(t, body, blen); break;    /* elst */
            case 0x6d646864: /* mdhd: version(1) flags(3) ... timescale */
                if (t && blen >= 4) { int ver = body[0]; int tsoff = ver == 1 ? 4 + 8 + 8 : 4 + 4 + 4; if (tsoff + 4 <= blen) t->timescale = (int)rd32(body + tsoff); }
                break;
            case 0x746b6864: /* tkhd: track id */
                if (t && blen >= 4) { int ver = body[0]; int idoff = ver == 1 ? 4 + 8 + 8 : 4 + 4 + 4; if (idoff + 4 <= blen) t->track_id = (int)rd32(body + idoff); }
                break;
            case 0x73747364: if (t) parse_stsd(t, body, blen); break;    /* stsd */
            /* classic sample tables (progressive MP4); zero-entry versions in
             * fMP4 init segments parse to nothing and stay inert. First box of
             * each kind wins — a duplicate in a malformed stbl would otherwise
             * overwrite (and leak) the earlier allocation. */
            case 0x73747473: if (t && !t->ctab.stts) t->ctab.stts = parse_table(body, blen, 2, 2, &t->ctab.stts_count); break;  /* stts */
            case 0x63747473: /* ctts: version 0 offsets are unsigned, version 1 signed */
                if (t && !t->ctab.ctts) {
                    t->ctab.ctts = parse_table(body, blen, 2, 2, &t->ctab.ctts_count);
                    t->ctab.ctts_version = blen >= 1 ? body[0] : 0;
                }
                break;
            case 0x73747363: if (t && !t->ctab.stsc) t->ctab.stsc = parse_table(body, blen, 3, 2, &t->ctab.stsc_count); break;  /* stsc */
            case 0x73747373: if (t && !t->ctab.stss) t->ctab.stss = parse_table(body, blen, 1, 1, &t->ctab.stss_count); break;  /* stss */
            case 0x7374737a: if (t && !t->ctab.sample_count) parse_stsz(&t->ctab, body, blen); break;                           /* stsz */
            case 0x73747a32: if (t) t->ctab.has_stz2 = 1; break;         /* stz2: compact sizes, unsupported */
            case 0x7374636f: if (t && !t->ctab.chunk_offsets) parse_chunk_offsets(&t->ctab, body, blen, 0); break;              /* stco */
            case 0x636f3634: if (t && !t->ctab.chunk_offsets) parse_chunk_offsets(&t->ctab, body, blen, 1); break;              /* co64 */
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

/* ---- classic (progressive) sample walk ---------------------------------- */

/* A track's tables are walkable when every table the walk depends on parsed
 * completely. stss/ctts are optional. */
static int classic_track_ready(const mp4_ctab_t* c) {
    return c->sample_count && c->chunk_count && c->stts_count && c->stsc_count &&
           (c->const_size || c->sizes);
}

static int classic_ready(mp4_t* m) {
    for (int i = 0; i < m->ntracks; ++i)
        if (classic_track_ready(&m->tracks[i].ctab)) return 1;
    return 0;
}

/* Longest track's stts total — the fallback when mvhd carries no duration. */
static int64_t classic_total_duration_us(mp4_t* m) {
    int64_t best = 0;
    for (int i = 0; i < m->ntracks; ++i) {
        const mp4_ctab_t* c = &m->tracks[i].ctab;
        if (!classic_track_ready(c)) continue;
        int64_t ticks = 0;
        for (uint32_t k = 0; k < c->stts_count; ++k)
            ticks += (int64_t)c->stts[k * 2] * c->stts[k * 2 + 1];
        int64_t us = ticks_to_us(ticks, m->tracks[i].timescale);
        if (us > best) best = us;
    }
    return best;
}

/* Resolve a track's edit list against the movie timescale: initial empty edits
 * (media_time -1) delay the whole track; the first normal edit's media_time is
 * the media-time origin (encoder priming / initial trim). Later edit segments
 * would need a segment-aware walk and are ignored. Samples ahead of the origin
 * keep their (negative) presentation time rather than being dropped — video
 * there can still be reference data the decoder needs. */
static void classic_apply_elst(mp4_t* m, mp4_track_t* t) {
    mp4_ctab_t* c = &t->ctab;
    for (uint32_t i = 0; i < t->elst_count; ++i) {
        if (t->elst[i].media_time < 0) {
            c->pts_delay_us += ticks_to_us((int64_t)t->elst[i].duration, m->movie_timescale > 0 ? m->movie_timescale : 1000);
            continue;
        }
        c->media_start = t->elst[i].media_time;
        break;
    }
}

static void classic_init_cursor(mp4_ctab_t* c) {
    if (!classic_track_ready(c)) { c->sample_count = 0; return; }
    c->next = 0;
    c->chunk = 0;
    c->chunk_sample = 0;
    c->offset = c->chunk_offsets[0];
    c->stsc_i = 0;
    c->stts_i = 0; c->stts_used = 0;
    c->ctts_i = 0; c->ctts_used = 0;
    c->stss_i = 0;
    c->dts = 0;
}

static uint32_t classic_size(const mp4_ctab_t* c, uint32_t i) {
    return c->const_size ? c->const_size : c->sizes[i];
}

static void classic_advance(mp4_ctab_t* c) {
    uint32_t size = classic_size(c, c->next);
    c->dts += (int64_t)c->stts[c->stts_i * 2 + 1];
    if (++c->stts_used >= c->stts[c->stts_i * 2] && c->stts_i + 1 < c->stts_count) {
        c->stts_i++; c->stts_used = 0;
    }
    if (c->ctts && ++c->ctts_used >= c->ctts[c->ctts_i * 2] && c->ctts_i + 1 < c->ctts_count) {
        c->ctts_i++; c->ctts_used = 0;
    }
    c->next++;
    c->chunk_sample++;
    if (c->chunk_sample >= c->stsc[c->stsc_i * 2 + 1]) {  /* samples-per-chunk of the current run */
        c->chunk++;
        c->chunk_sample = 0;
        while (c->stsc_i + 1 < c->stsc_count && c->chunk + 1 >= c->stsc[(c->stsc_i + 1) * 2])
            c->stsc_i++;
        if (c->chunk < c->chunk_count) c->offset = c->chunk_offsets[c->chunk];
    } else {
        c->offset += size;
    }
}

/* A sample read from the file but not yet handed to the sink: file order can
 * run ahead of delivery order when the muxer interleaves chunks coarsely.
 * Video is stored already converted to Annex B with its keyframe verdict. */
typedef struct mp4_pend {
    struct mp4_pend* next;
    int64_t pts_us;
    int64_t dts_us;
    int     key;
    int     size;
    uint8_t data[];
} mp4_pend_t;

#define MP4_MAX_PENDING (32LL * 1024 * 1024)  /* parked-bytes bound; past it deliver anyway */

/* Decode time of the cursor's next sample, mapped like emission. */
static int64_t classic_sample_dts_us(const mp4_track_t* t) {
    const mp4_ctab_t* c = &t->ctab;
    return ticks_to_us(c->dts - c->media_start, t->timescale) + c->pts_delay_us;
}

/* Next undelivered decode time for a track: the parked front, else the table
 * cursor's sample while it still points inside this mdat. */
static int64_t classic_next_due(const mp4_track_t* t, const mp4_pend_t* head, int64_t mdat_end) {
    if (head) return head->dts_us;
    const mp4_ctab_t* c = &t->ctab;
    if (c->next >= c->sample_count || (int64_t)c->offset >= mdat_end) return INT64_MAX;
    return classic_sample_dts_us(t);
}

static void emit_pend(mp4_t* m, const mp4_track_t* t, const mp4_pend_t* s) {
    if (t->is_video) m->sink->on_video_au(m->sink->user, s->data, s->size, s->pts_us, s->dts_us, s->key);
    else m->sink->on_audio_frame(m->sink->user, s->data, s->size, s->pts_us);
}

/* Walks the classic tables against one mdat. The byte source only moves
 * forward, so samples are read in file-offset order — the muxer's chunk
 * interleave — but delivered in decode-time order across tracks: with a
 * coarse interleave (a chunk of video ahead of the matching audio) the
 * delivery-paced chunk in front would otherwise hold the other track's
 * earlier samples past their turn and starve it. Samples whose file order
 * runs ahead of their delivery turn are parked in per-track queues, bounded
 * by MP4_MAX_PENDING — past the bound (or once nothing more can be read from
 * this mdat) the earliest parked sample delivers regardless. Samples whose
 * offsets lie beyond this mdat wait for a later one. mdat_end is the absolute
 * end of the payload, or INT64_MAX for a size-0 box. */
static int consume_progressive(mp4_t* m, int64_t mdat_end) {
    uint8_t* buf = NULL;
    size_t cap = 0;
    int rc = 0;
    mp4_pend_t* head[2] = { NULL, NULL };
    mp4_pend_t* tail[2] = { NULL, NULL };
    int64_t queued = 0;   /* payload bytes parked across both queues */

    for (;;) {
        if (!m->sink->is_running(m->sink->user)) { rc = -1; break; }

        /* the next sample in file order, while any remains in this mdat */
        mp4_track_t* rt = NULL;
        for (int i = 0; i < m->ntracks; ++i) {
            mp4_ctab_t* c = &m->tracks[i].ctab;
            if (c->next >= c->sample_count) continue;
            if ((int64_t)c->offset >= mdat_end) continue;
            if (!rt || c->offset < rt->ctab.offset) rt = &m->tracks[i];
        }

        /* deliver the earliest parked sample once no other track can still
         * produce an earlier one — or when forced (bound hit, or nothing left
         * to read in this mdat) */
        int e = -1;
        for (int i = 0; i < m->ntracks; ++i)
            if (head[i] && (e < 0 || head[i]->dts_us < head[e]->dts_us)) e = i;
        if (e >= 0) {
            int64_t barrier = INT64_MAX;
            for (int i = 0; i < m->ntracks; ++i) {
                if (i == e) continue;
                int64_t due = classic_next_due(&m->tracks[i], head[i], mdat_end);
                if (due < barrier) barrier = due;
            }
            if (head[e]->dts_us <= barrier || queued > MP4_MAX_PENDING || !rt) {
                mp4_pend_t* s = head[e];
                head[e] = s->next;
                if (!head[e]) tail[e] = NULL;
                queued -= s->size;
                emit_pend(m, &m->tracks[e], s);
                free(s);
                continue;
            }
        }
        if (!rt) break;

        mp4_ctab_t* c = &rt->ctab;
        uint32_t ssize = classic_size(c, c->next);
        if (ssize == 0 || ssize > (uint32_t)MP4_MAX_BOX || (int64_t)c->offset < m->pos ||
            (mdat_end != INT64_MAX && (int64_t)c->offset > mdat_end - (int64_t)ssize)) {
            /* behind the stream head or overrunning the box: the table points at
             * bytes we can't reach — drop the sample and keep walking */
            classic_advance(c);
            continue;
        }
        if (skip_bytes(m, (int64_t)c->offset - m->pos) != 0) { rc = -1; break; }
        if (ssize > cap) {
            free(buf);
            cap = (size_t)ssize + ((size_t)ssize >> 1) + 4096;
            buf = (uint8_t*)malloc(cap);
            if (!buf) { cap = 0; rc = -1; break; }
        }
        if (read_exact(m, buf, (int)ssize) != (int)ssize) { rc = -1; break; }

        int64_t cto = 0;
        if (c->ctts) {
            uint32_t raw = c->ctts[c->ctts_i * 2 + 1];
            cto = c->ctts_version ? (int64_t)(int32_t)raw : (int64_t)raw;
        }
        int64_t pts_us = ticks_to_us(c->dts + cto - c->media_start, rt->timescale) + c->pts_delay_us;
        int64_t dts_us = classic_sample_dts_us(rt);

        mp4_pend_t* s = NULL;
        if (rt->is_video) {
            int key = 1;
            if (c->stss) {
                /* stss is sorted 1-based sample numbers; catch the cursor up past
                 * any entries skipped by dropped samples before comparing */
                while (c->stss_i < c->stss_count && c->stss[c->stss_i] < c->next + 1) c->stss_i++;
                key = c->stss_i < c->stss_count && c->stss[c->stss_i] == c->next + 1;
                if (key) c->stss_i++;
            }
            int nls = rt->nal_len_size ? rt->nal_len_size : 4;
            int outcap = basis_avcc_annexb_cap((int)ssize, nls);
            s = (mp4_pend_t*)malloc(sizeof(*s) + (size_t)outcap);
            if (s) {
                int n = basis_avcc_to_annexb(buf, (int)ssize, nls, s->data, outcap);
                if (n > 0) {
                    if (c->stss == NULL)
                        key = rt->codec == BASIS_CODEC_H265 ? basis_h265_is_keyframe(s->data, n) : basis_h264_is_keyframe(s->data, n);
                    s->size = n;
                    s->key = key;
                } else { free(s); s = NULL; }
            }
        } else {
            s = (mp4_pend_t*)malloc(sizeof(*s) + ssize);
            if (s) { memcpy(s->data, buf, ssize); s->size = (int)ssize; s->key = 0; }
        }
        if (s) {
            int ti = (int)(rt - m->tracks);
            s->pts_us = pts_us;
            s->dts_us = dts_us;
            s->next = NULL;
            if (tail[ti]) tail[ti]->next = s; else head[ti] = s;
            tail[ti] = s;
            queued += s->size;
        }
        classic_advance(c);
    }

    /* the loop drains the queues before exiting cleanly; anything still parked
     * here means the engine is stopping — release without delivering */
    for (int i = 0; i < 2; ++i)
        while (head[i]) { mp4_pend_t* s = head[i]; head[i] = s->next; free(s); }

    free(buf);
    if (rc == 0 && mdat_end != INT64_MAX && m->pos < mdat_end)
        return skip_bytes(m, mdat_end - m->pos);
    return rc;
}

/* ---- moof parsing ------------------------------------------------------- */

static int frag_reserve(mp4_frag_t* f, int n) {
    if (n <= f->cap) return 1;
    int nc = f->cap ? f->cap * 2 : 256;
    while (nc < n) nc *= 2;
    uint32_t* s = (uint32_t*)realloc(f->sizes, (size_t)nc * sizeof(uint32_t));
    if (s) f->sizes = s;
    uint32_t* d = (uint32_t*)realloc(f->durs, (size_t)nc * sizeof(uint32_t));
    if (d) f->durs = d;
    int32_t*  c = (int32_t*)realloc(f->ctos, (size_t)nc * sizeof(int32_t));
    if (c) f->ctos = c;
    if (!s || !d || !c) return 0;
    f->cap = nc;
    return 1;
}

/* Parse one traf into a fragment run per trun. tfhd/tfdt give the track and base
 * decode time; each trun gives a data-offset (relative to the moof) and samples. */
static void parse_traf(mp4_t* m, const uint8_t* p, int len) {
    int off = 0;
    int track_id = 0;
    uint32_t default_dur = 0, default_size = 0;
    int64_t base_dts = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || sz > len - off) break;
        const uint8_t* b = p + off + 8;
        int blen = sz - 8;
        if (ty == 0x74666864 && blen >= 8) { /* tfhd */
            uint32_t flags = rd32(b) & 0xFFFFFF;
            track_id = (int)rd32(b + 4);
            int q = 8;
            if (flags & 0x000001) q += 8;  /* base-data-offset */
            if (flags & 0x000002) q += 4;  /* sample-description-index */
            if (flags & 0x000008) { if (q + 4 > blen) { off += sz; continue; } default_dur = rd32(b + q); q += 4; }
            if (flags & 0x000010) { if (q + 4 > blen) { off += sz; continue; } default_size = rd32(b + q); q += 4; }
        } else if (ty == 0x74666474 && blen >= 8) { /* tfdt */
            int ver = b[0];
            if (ver != 1) base_dts = (int64_t)rd32(b + 4);
            else if (blen >= 12) base_dts = (int64_t)rd64(b + 4);
        } else if (ty == 0x7472756e && m->nfrags < MP4_MAX_FRAGS) { /* trun */
            if (blen < 8) { off += sz; continue; }
            mp4_frag_t* f = &m->frags[m->nfrags];
            uint32_t flags = rd32(b) & 0xFFFFFF;
            uint32_t count = rd32(b + 4);
            int q = 8;
            int data_offset = 0;
            if (flags & 0x000001) { if (q + 4 > blen) { off += sz; continue; } data_offset = (int)rd32(b + q); q += 4; } /* data-offset */
            if (flags & 0x000004) { if (q + 4 > blen) { off += sz; continue; } q += 4; } /* first-sample-flags */
            /* the declared per-sample table must fit inside this box */
            int per = 4 * (!!(flags & 0x000100) + !!(flags & 0x000200) + !!(flags & 0x000400) + !!(flags & 0x000800));
            if (count == 0 || count > MP4_MAX_FRAG_SAMPLES ||
                (int64_t)count * per > (int64_t)blen - q) { off += sz; continue; }
            if (!frag_reserve(f, (int)count)) { off += sz; continue; }
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t dur = default_dur, size = default_size;
                int32_t cto = 0;
                if (flags & 0x000100) { dur = rd32(b + q); q += 4; }
                if (flags & 0x000200) { size = rd32(b + q); q += 4; }
                if (flags & 0x000400) { q += 4; }            /* sample flags */
                if (flags & 0x000800) { cto = (int32_t)rd32(b + q); q += 4; }
                f->sizes[i] = size; f->durs[i] = dur; f->ctos[i] = cto;
            }
            f->track_id = track_id;
            f->base_dts = base_dts;
            f->data_offset = data_offset;
            f->default_dur = default_dur;
            f->count = (int)count;
            m->nfrags++;
        }
        off += sz;
    }
}

static void parse_moof(mp4_t* m, const uint8_t* p, int len) {
    m->nfrags = 0;
    int off = 0;
    while (off + 8 <= len) {
        int sz = (int)rd32(p + off);
        uint32_t ty = rd32(p + off + 4);
        if (sz < 8 || sz > len - off) break;
        if (ty == 0x74726166) parse_traf(m, p + off + 8, sz - 8); /* traf */
        off += sz;
    }
}

/* A moof's trafs share one mdat. Each trun's data-offset is relative to the moof
 * start; the smallest maps to the first byte of this mdat's payload, so subtract
 * it to place each run within the buffer we were handed.
 *
 * The runs are emitted as a DTS-ordered merge, not back to back: a run is one
 * track's whole fragment, so run-at-a-time emission would hand the decoder a
 * fragment of video before any of that fragment's audio — and with delivery
 * pacing on (VOD, HLS) the paced video burst makes the trailing audio arrive a
 * full fragment late, starving the audio-gated presentation clock. Merging by
 * decode time interleaves the tracks at frame granularity, like the TS demuxer,
 * and each sample's decode time rides along to the sink so the pacing gate
 * holds it no later than its decode turn. Order within a run (decode order)
 * is preserved. */
static void consume_mdat(mp4_t* m, const uint8_t* data, int len) {
    if (m->nfrags <= 0) return;
    int base_off = m->frags[0].data_offset;
    for (int k = 1; k < m->nfrags; ++k)
        if (m->frags[k].data_offset < base_off) base_off = m->frags[k].data_offset;

    mp4_track_t* trk[MP4_MAX_FRAGS];
    int64_t pos[MP4_MAX_FRAGS];
    int     idx[MP4_MAX_FRAGS];
    int64_t dts[MP4_MAX_FRAGS];
    for (int k = 0; k < m->nfrags; ++k) {
        const mp4_frag_t* f = &m->frags[k];
        trk[k] = track_by_id(m, f->track_id);
        pos[k] = (int64_t)f->data_offset - base_off;
        idx[k] = (trk[k] && pos[k] >= 0) ? 0 : f->count; /* unknown track / bad offset: skip run */
        dts[k] = f->base_dts;
    }

    for (;;) {
        int k = -1;
        int64_t best_us = 0;
        for (int j = 0; j < m->nfrags; ++j) {
            if (idx[j] >= m->frags[j].count) continue;
            int64_t us = ticks_to_us(dts[j], trk[j]->timescale);
            if (k < 0 || us < best_us) { k = j; best_us = us; }
        }
        if (k < 0) break;

        const mp4_frag_t* f = &m->frags[k];
        mp4_track_t* t = trk[k];
        int i = idx[k];
        uint32_t ssize = f->sizes[i];
        if (ssize == 0 || ssize > (uint32_t)len || pos[k] > (int64_t)len - (int64_t)ssize) {
            idx[k] = f->count; continue; /* run truncated */
        }
        int64_t pts_us = ticks_to_us(dts[k] + f->ctos[i], t->timescale);

        if (t->is_video) {
            int nls = t->nal_len_size ? t->nal_len_size : 4;
            int cap = basis_avcc_annexb_cap((int)ssize, nls);
            uint8_t* out = (uint8_t*)malloc((size_t)cap);
            if (out) {
                int n = basis_avcc_to_annexb(data + pos[k], (int)ssize, nls, out, cap);
                if (n > 0) {
                    int key = t->codec == BASIS_CODEC_H265 ? basis_h265_is_keyframe(out, n) : basis_h264_is_keyframe(out, n);
                    m->sink->on_video_au(m->sink->user, out, n, pts_us, best_us, key);
                }
                free(out);
            }
        } else {
            m->sink->on_audio_frame(m->sink->user, data + pos[k], (int)ssize, pts_us);
        }

        pos[k] += ssize;
        dts[k] += f->durs[i] ? f->durs[i] : f->default_dur;
        idx[k]++;
    }
}

int basis_mp4_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx) {
    mp4_t m; memset(&m, 0, sizeof(m));
    m.sink = sink; m.read = read; m.ctx = ctx;

    while (sink->is_running(sink->user)) {
        uint32_t type; int64_t body;
        if (read_box_header(&m, &type, &body) != 0) break;

        if (type == 0x6d646174) { /* mdat */
            if (classic_ready(&m)) {
                /* progressive: stream the payload sample-by-sample (no size cap) */
                int64_t end = body < 0 ? INT64_MAX : m.pos + body;
                if (consume_progressive(&m, end) != 0) break;
                continue;
            }
            if (m.nfrags > 0 && body >= 0 && body <= MP4_MAX_BOX) {
                /* fragment payload: small — buffer whole and slice by the moof's runs */
                uint8_t* buf = (uint8_t*)malloc((size_t)(body ? body : 1));
                if (!buf) break;
                if (read_exact(&m, buf, (int)body) != (int)body) { free(buf); break; }
                consume_mdat(&m, buf, (int)body);
                free(buf);
                continue;
            }
            /* media data with no index yet: the moov may still follow (trailing-
             * moov progressive file). Skip it, and report if the moov proves that
             * these bytes were the media. */
            m.mdat_skipped = 1;
            if (body < 0 || skip_bytes(&m, body) != 0) break;
            continue;
        }

        if (body < 0 || body > MP4_MAX_BOX) break;
        uint8_t* buf = (uint8_t*)malloc((size_t)(body ? body : 1));
        if (!buf) break;
        if (read_exact(&m, buf, (int)body) != (int)body) { free(buf); break; }

        switch (type) {
            case 0x6d6f6f76: /* moov */
                parse_box_tree(&m, NULL, buf, (int)body);
                for (int i = 0; i < m.ntracks; ++i) {
                    classic_apply_elst(&m, &m.tracks[i]);
                    classic_init_cursor(&m.tracks[i].ctab);
                }
                announce_tracks(&m);
                /* A track whose sizes live in stz2 has no walkable tables; playing
                 * the remaining track would silently drop this one, so fail loudly. */
                for (int i = 0; i < m.ntracks; ++i)
                    if (m.tracks[i].ctab.has_stz2 && !classic_track_ready(&m.tracks[i].ctab)) {
                        sink->on_error(sink->user,
                            "MP4 uses a compact (stz2) sample-size table, which isn't supported; remux (ffmpeg -c copy) to rewrite the sample tables");
                        break;
                    }
                if (m.mdat_skipped && classic_ready(&m))
                    sink->on_error(sink->user,
                        "progressive MP4 stores its index (moov) after the media data, which can't play over a one-way stream; remux with faststart (ffmpeg -movflags +faststart)");
                /* Progressive files have a complete timeline in hand — report it.
                 * (fMP4 durations come from the layer that knows them, e.g. an HLS
                 * VOD playlist; an init-segment mvhd is usually zero anyway.) */
                if (classic_ready(&m) && sink->on_duration) {
                    int64_t dur_us = m.movie_duration > 0
                        ? ticks_to_us(m.movie_duration, m.movie_timescale > 0 ? m.movie_timescale : 1000)
                        : classic_total_duration_us(&m);
                    if (dur_us > 0) sink->on_duration(sink->user, dur_us);
                }
                break;
            case 0x6d6f6f66: /* moof */
                parse_moof(&m, buf, (int)body);
                break;
            default:
                break; /* ftyp, styp, sidx, free, ... ignored */
        }
        free(buf);
    }

    for (int k = 0; k < MP4_MAX_FRAGS; ++k) {
        free(m.frags[k].sizes); free(m.frags[k].durs); free(m.frags[k].ctos);
    }
    for (int i = 0; i < 2; ++i) {
        mp4_ctab_t* c = &m.tracks[i].ctab;
        free(c->sizes); free(c->stts); free(c->ctts);
        free(c->stsc); free(c->chunk_offsets); free(c->stss);
    }
    return 0;
}
