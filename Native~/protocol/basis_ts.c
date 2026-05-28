/*
 * basis_ts.c — MPEG-TS demuxer (PAT/PMT/PES) -> H.264/H.265 + AAC.
 *
 * Live TS over HTTP is Annex-B video + ADTS audio in 188-byte packets. We resync
 * on 0x47, follow PAT->PMT to find the elementary PIDs, reassemble PES per PID,
 * and push access units / AAC frames into the sink. PCR/PTS are 90 kHz.
 */

#include "basis_ts.h"
#include "basis_bitstream.h"

#include <stdlib.h>
#include <string.h>

#define TS_PKT 188

typedef struct {
    int pid;
    basis_codec_t codec;
    uint8_t* buf;
    int len, cap;
    int64_t pts_us;
    int started;
} es_accum_t;

typedef struct {
    basis_media_sink_t* sink;
    basis_read_fn read;
    void* ctx;

    int pmt_pid;
    int video_pid;
    int audio_pid;
    basis_codec_t video_codec;
    basis_codec_t audio_codec;

    es_accum_t v;
    es_accum_t a;

    int audio_announced;
    int audio_sr, audio_ch, audio_profile;
    int video_announced;
} ts_t;

static int accum_reserve(es_accum_t* e, int extra) {
    if (e->len + extra <= e->cap) return 1;
    int ncap = e->cap ? e->cap * 2 : 65536;
    while (ncap < e->len + extra) ncap *= 2;
    uint8_t* nb = (uint8_t*)realloc(e->buf, (size_t)ncap);
    if (!nb) return 0;
    e->buf = nb; e->cap = ncap;
    return 1;
}

static int64_t pts_to_us(int64_t pts90) { return pts90 * 1000 / 90; }

static int64_t parse_pes_pts(const uint8_t* p, int len, int* payload_off) {
    /* p points at the PES packet start (00 00 01 stream_id ...) */
    if (len < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1) { *payload_off = 0; return -1; }
    int flags = (p[7] >> 6) & 0x3;
    int hdr_len = p[8];
    int64_t pts = -1;
    if ((flags & 0x2) && len >= 14) {
        pts = ((int64_t)(p[9] >> 1 & 0x7) << 30) |
              ((int64_t)p[10] << 22) |
              ((int64_t)(p[11] >> 1) << 15) |
              ((int64_t)p[12] << 7) |
              ((int64_t)(p[13] >> 1));
    }
    *payload_off = 9 + hdr_len;
    return pts;
}

static void flush_video(ts_t* t) {
    es_accum_t* e = &t->v;
    if (!e->started || e->len <= 0) { e->len = 0; e->started = 0; return; }

    int off = 0;
    int64_t pts = parse_pes_pts(e->buf, e->len, &off);
    if (off < 0 || off >= e->len) { e->len = 0; e->started = 0; return; }

    const uint8_t* au = e->buf + off;
    int au_len = e->len - off;
    int64_t pts_us = pts >= 0 ? pts_to_us(pts) : e->pts_us;

    if (!t->video_announced) {
        int w = 0, h = 0;
        if (t->video_codec == BASIS_CODEC_H264) {
            int pos = 0, no, nl;
            while ((pos = basis_annexb_next(au, au_len, pos, &no, &nl)) >= 0) {
                if (nl > 0 && basis_h264_nal_type(au[no]) == 7) { basis_h264_sps_dimensions(au + no, nl, &w, &h); break; }
            }
        }
        t->sink->on_video_format(t->sink->user, t->video_codec, NULL, 0, w, h);
        t->video_announced = 1;
    }

    int key = (t->video_codec == BASIS_CODEC_H265) ? basis_h265_is_keyframe(au, au_len)
                                                   : basis_h264_is_keyframe(au, au_len);
    t->sink->on_video_au(t->sink->user, au, au_len, pts_us, key);

    e->len = 0;
    e->started = 0;
}

static void flush_audio(ts_t* t) {
    es_accum_t* e = &t->a;
    if (!e->started || e->len <= 0) { e->len = 0; e->started = 0; return; }

    int off = 0;
    int64_t pts = parse_pes_pts(e->buf, e->len, &off);
    if (off < 0 || off >= e->len) { e->len = 0; e->started = 0; return; }

    const uint8_t* p = e->buf + off;
    int remain = e->len - off;
    int64_t base_us = pts >= 0 ? pts_to_us(pts) : e->pts_us;
    int frame_idx = 0;

    while (remain >= 7) {
        basis_adts_t ad;
        if (basis_adts_parse(p, remain, &ad) != 0) break;
        if (ad.frame_len > remain) break;

        if (!t->audio_announced) {
            uint8_t asc[2];
            basis_aac_build_asc(ad.profile + 1, ad.sample_rate, ad.channels, asc);
            t->audio_sr = ad.sample_rate; t->audio_ch = ad.channels; t->audio_profile = ad.profile;
            t->sink->on_audio_format(t->sink->user, BASIS_CODEC_AAC, ad.sample_rate, ad.channels, asc, 2);
            t->audio_announced = 1;
        }

        int raw_len = ad.frame_len - ad.header_len;
        int64_t fpts = base_us + (int64_t)frame_idx * 1024 * 1000000 / (ad.sample_rate > 0 ? ad.sample_rate : 48000);
        t->sink->on_audio_frame(t->sink->user, p + ad.header_len, raw_len, fpts);

        p += ad.frame_len;
        remain -= ad.frame_len;
        frame_idx++;
    }

    e->len = 0;
    e->started = 0;
}

static void feed_es(ts_t* t, es_accum_t* e, int pusi, const uint8_t* payload, int plen, int is_video) {
    if (pusi) {
        if (is_video) flush_video(t); else flush_audio(t);
        e->started = 1;
    }
    if (!e->started) return;
    if (!accum_reserve(e, plen)) return;
    memcpy(e->buf + e->len, payload, (size_t)plen);
    e->len += plen;
}

static void parse_pat(ts_t* t, const uint8_t* p, int plen) {
    if (plen < 1) return;
    int ptr = p[0];                 /* pointer field */
    const uint8_t* s = p + 1 + ptr;
    int avail = plen - 1 - ptr;
    if (avail < 8) return;
    int section_len = ((s[1] & 0x0F) << 8) | s[2];
    int n = section_len - 5 - 4;    /* minus header-after-len and CRC */
    const uint8_t* prog = s + 8;
    for (int i = 0; i + 4 <= n; i += 4) {
        int program = (prog[i] << 8) | prog[i + 1];
        int pid = ((prog[i + 2] & 0x1F) << 8) | prog[i + 3];
        if (program != 0) { t->pmt_pid = pid; break; } /* first real program */
    }
}

static void parse_pmt(ts_t* t, const uint8_t* p, int plen) {
    if (plen < 1) return;
    int ptr = p[0];
    const uint8_t* s = p + 1 + ptr;
    int avail = plen - 1 - ptr;
    if (avail < 12) return;
    int section_len = ((s[1] & 0x0F) << 8) | s[2];
    int prog_info_len = ((s[10] & 0x0F) << 8) | s[11];
    const uint8_t* es = s + 12 + prog_info_len;
    const uint8_t* end = s + 3 + section_len - 4; /* up to CRC */
    while (es + 5 <= end) {
        int stype = es[0];
        int pid = ((es[1] & 0x1F) << 8) | es[2];
        int eslen = ((es[3] & 0x0F) << 8) | es[4];
        if ((stype == 0x1B) && t->video_pid < 0) { t->video_pid = pid; t->video_codec = BASIS_CODEC_H264; t->v.pid = pid; }
        else if ((stype == 0x24) && t->video_pid < 0) { t->video_pid = pid; t->video_codec = BASIS_CODEC_H265; t->v.pid = pid; }
        else if ((stype == 0x0F || stype == 0x11) && t->audio_pid < 0) { t->audio_pid = pid; t->audio_codec = BASIS_CODEC_AAC; t->a.pid = pid; }
        es += 5 + eslen;
    }
}

static void handle_packet(ts_t* t, const uint8_t* pkt) {
    if (pkt[0] != 0x47) return;
    int pusi = (pkt[1] >> 6) & 0x1;
    int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    int afc = (pkt[3] >> 4) & 0x3;
    int has_payload = afc & 0x1;
    if (!has_payload) return;

    int off = 4;
    if (afc & 0x2) { int al = pkt[4]; off = 5 + al; }
    if (off >= TS_PKT) return;
    const uint8_t* payload = pkt + off;
    int plen = TS_PKT - off;

    if (pid == 0) { parse_pat(t, payload, plen); return; }
    if (pid == t->pmt_pid && t->pmt_pid > 0) { parse_pmt(t, payload, plen); return; }
    if (pid == t->video_pid && t->video_pid > 0) { feed_es(t, &t->v, pusi, payload, plen, 1); return; }
    if (pid == t->audio_pid && t->audio_pid > 0) { feed_es(t, &t->a, pusi, payload, plen, 0); return; }
}

int basis_ts_run(basis_media_sink_t* sink, basis_read_fn read, void* ctx) {
    ts_t t;
    memset(&t, 0, sizeof(t));
    t.sink = sink; t.read = read; t.ctx = ctx;
    t.pmt_pid = -1; t.video_pid = -1; t.audio_pid = -1;

    uint8_t rb[TS_PKT * 16];
    int rb_len = 0;

    while (sink->is_running(sink->user)) {
        int space = (int)sizeof(rb) - rb_len;
        if (space <= 0) { rb_len = 0; space = (int)sizeof(rb); } /* desync guard */
        int n = read(ctx, rb + rb_len, space);
        if (n <= 0) break;
        rb_len += n;

        int i = 0;
        while (rb_len - i >= TS_PKT) {
            if (rb[i] != 0x47) { i++; continue; }            /* resync */
            if (rb_len - i < TS_PKT) break;
            handle_packet(&t, rb + i);
            i += TS_PKT;
        }
        /* keep the tail */
        if (i > 0) { memmove(rb, rb + i, (size_t)(rb_len - i)); rb_len -= i; }
    }

    flush_video(&t);
    flush_audio(&t);
    free(t.v.buf);
    free(t.a.buf);
    return 0;
}
