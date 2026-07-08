/*
 * basis_rtsp.c — RTSP client with RTP interleaved over the TCP control channel
 * (the "rtspt" transport: Transport: RTP/AVP/TCP;interleaved=0-1).
 *
 * Flow: OPTIONS -> DESCRIBE (parse SDP) -> SETUP (per media) -> PLAY, then read
 * interleaved RTP. Video is depacketized (H.264 single/STAP-A/FU-A, H.265
 * single/AP/FU) into Annex B access units; AAC (MPEG4-GENERIC) into raw frames.
 *
 * Scope notes: Basic auth only (no Digest); one video + one audio media. These
 * cover VRCDN-style public endpoints; Digest/auth-heavy servers need a follow-up.
 */

#include "basis_rtsp.h"
#include "basis_io.h"
#include "basis_bitstream.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#if defined(_WIN32)
  #define strncasecmp _strnicmp
#else
  #include <strings.h>
#endif

/* ---- base64 ------------------------------------------------------------- */

static int b64dec(const char* in, int inlen, uint8_t* out, int outcap) {
    int8_t tab[256];
    for (int i = 0; i < 256; ++i) tab[i] = -1;
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; ++i) tab[(unsigned char)a[i]] = (int8_t)i;

    int val = 0, bits = 0, op = 0;
    for (int i = 0; i < inlen; ++i) {
        int c = (unsigned char)in[i];
        if (c == '=' || tab[c] < 0) continue;
        val = (val << 6) | tab[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (op < outcap) out[op++] = (uint8_t)((val >> bits) & 0xFF);
        }
    }
    return op;
}

/* ---- RTSP request/response --------------------------------------------- */

typedef struct {
    basis_io_t* io;
    int cseq;
    char session[128];
    char authb64[256];   /* Basic auth value, or empty */
    char last_status[160]; /* last response status line, for diagnostics */
    char www_auth[256];    /* last WWW-Authenticate header value, if any */
    char location[1024];   /* last Location header (redirects) */
} rtsp_t;

static int rtsp_send(rtsp_t* r, const char* method, const char* url, const char* extra) {
    char req[2048];
    int n = snprintf(req, sizeof(req),
        "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: BasisMediaPlayer/1.0\r\n",
        method, url, ++r->cseq);
    if (r->session[0]) n += snprintf(req + n, sizeof(req) - n, "Session: %s\r\n", r->session);
    if (r->authb64[0]) n += snprintf(req + n, sizeof(req) - n, "Authorization: Basic %s\r\n", r->authb64);
    if (extra) n += snprintf(req + n, sizeof(req) - n, "%s", extra);
    n += snprintf(req + n, sizeof(req) - n, "\r\n");
    return basis_io_write_full(r->io, (const uint8_t*)req, n) == n ? 0 : -1;
}

/* Reads an RTSP response: status + headers, then body by Content-Length.
 * Returns status code, copies body (<=bodycap) and Session id if present. */
static int rtsp_recv(rtsp_t* r, char* body, int bodycap, int* bodylen) {
    char line[1024];
    int code = -1, content_len = 0;
    int li = 0;
    r->last_status[0] = 0;
    r->www_auth[0] = 0;
    r->location[0] = 0;
    /* header lines until blank */
    for (;;) {
        li = 0;
        for (;;) {
            uint8_t c;
            if (basis_io_read_full(r->io, &c, 1) != 1) return -1;
            if (c == '\n') break;
            if (c != '\r' && li < (int)sizeof(line) - 1) line[li++] = (char)c;
        }
        line[li] = 0;
        if (li == 0) break; /* end of headers */
        if (code < 0 && strncmp(line, "RTSP/1.0", 8) == 0) {
            code = atoi(line + 9);
            strncpy(r->last_status, line, sizeof(r->last_status) - 1);
            r->last_status[sizeof(r->last_status) - 1] = 0;
        }
        else if (strncasecmp(line, "Content-Length:", 15) == 0) content_len = atoi(line + 15);
        else if (strncasecmp(line, "WWW-Authenticate:", 17) == 0) {
            const char* v = line + 17; while (*v == ' ') v++;
            strncpy(r->www_auth, v, sizeof(r->www_auth) - 1);
            r->www_auth[sizeof(r->www_auth) - 1] = 0;
        }
        else if (strncasecmp(line, "Location:", 9) == 0) {
            const char* v = line + 9; while (*v == ' ') v++;
            strncpy(r->location, v, sizeof(r->location) - 1);
            r->location[sizeof(r->location) - 1] = 0;
        }
        else if (strncasecmp(line, "Session:", 8) == 0) {
            const char* s = line + 8; while (*s == ' ') s++;
            int j = 0; while (s[j] && s[j] != ';' && j < (int)sizeof(r->session) - 1) { r->session[j] = s[j]; j++; }
            r->session[j] = 0;
        }
    }
    if (bodylen) *bodylen = 0;
    if (content_len > 0) {
        int want = body ? (content_len < bodycap ? content_len : bodycap) : 0;
        int got = want > 0 ? basis_io_read_full(r->io, (uint8_t*)body, want) : 0;
        if (bodylen) *bodylen = got;
        /* drain whatever the caller's buffer didn't take, so an ignored or
         * oversized body can't desynchronise the next reply on the socket */
        for (int rest = content_len - got; rest > 0; ) {
            uint8_t tmp[256]; int t = rest < (int)sizeof(tmp) ? rest : (int)sizeof(tmp);
            if (basis_io_read_full(r->io, tmp, t) != t) break;
            rest -= t;
        }
    }
    return code;
}

/* ---- SDP parse --------------------------------------------------------- */

typedef struct {
    basis_codec_t codec;
    int pt;            /* RTP payload type */
    int clock;         /* RTP clock rate */
    int channels;      /* audio */
    char control[512]; /* a=control */
    uint8_t extradata[1024];
    int extradata_len;
    uint8_t asc[16];
    int asc_len;
} sdp_media_t;

static void append_param_set_b64(sdp_media_t* m, const char* b64, int len) {
    static const uint8_t sc[4] = {0,0,0,1};
    uint8_t tmp[512];
    int n = b64dec(b64, len, tmp, sizeof(tmp));
    if (n <= 0) return;
    if (m->extradata_len + 4 + n > (int)sizeof(m->extradata)) return;
    memcpy(m->extradata + m->extradata_len, sc, 4); m->extradata_len += 4;
    memcpy(m->extradata + m->extradata_len, tmp, n); m->extradata_len += n;
}

static int hexval(int c){ if(c>='0'&&c<='9')return c-'0'; c|=32; if(c>='a'&&c<='f')return c-'a'+10; return -1; }

static void parse_fmtp(sdp_media_t* m, const char* fmtp) {
    /* H.264: sprop-parameter-sets=<sps_b64>,<pps_b64> */
    const char* sp = strstr(fmtp, "sprop-parameter-sets=");
    if (sp) {
        sp += 21;
        const char* comma = strchr(sp, ',');
        const char* end = sp; while (*end && *end != ';' && *end != '\r' && *end != '\n') end++;
        if (comma && comma < end) {
            append_param_set_b64(m, sp, (int)(comma - sp));
            append_param_set_b64(m, comma + 1, (int)(end - (comma + 1)));
        } else {
            append_param_set_b64(m, sp, (int)(end - sp));
        }
    }
    /* H.265: sprop-vps / sprop-sps / sprop-pps */
    const char* tags[3] = { "sprop-vps=", "sprop-sps=", "sprop-pps=" };
    for (int i = 0; i < 3; ++i) {
        const char* t = strstr(fmtp, tags[i]);
        if (t) { t += strlen(tags[i]); const char* end = t; while (*end && *end != ';' && *end != '\r' && *end != '\n') end++; append_param_set_b64(m, t, (int)(end - t)); }
    }
    /* AAC MPEG4-GENERIC: config=<hex ASC> */
    const char* cfg = strstr(fmtp, "config=");
    if (cfg) {
        cfg += 7; int n = 0;
        while (cfg[0] && cfg[1] && cfg[0] != ';' && n < (int)sizeof(m->asc)) {
            int hi = hexval(cfg[0]), lo = hexval(cfg[1]);
            if (hi < 0 || lo < 0) break;
            m->asc[n++] = (uint8_t)((hi << 4) | lo); cfg += 2;
        }
        m->asc_len = n;
    }
}

/* Parses SDP into up to one video + one audio media. Returns count. */
static int parse_sdp(const char* sdp, int len, sdp_media_t* video, sdp_media_t* audio) {
    int have_v = 0, have_a = 0;
    sdp_media_t* cur = NULL;
    const char* p = sdp; const char* end = sdp + len;
    char line[1024];
    while (p < end) {
        int li = 0;
        while (p < end && *p != '\n' && li < (int)sizeof(line) - 1) { if (*p != '\r') line[li++] = *p; p++; }
        if (p < end) p++;
        line[li] = 0;
        if (line[0] == 'm' && line[1] == '=') {
            if (strncmp(line + 2, "video", 5) == 0 && !have_v) { cur = video; memset(cur, 0, sizeof(*cur)); cur->codec = BASIS_CODEC_H264; cur->clock = 90000; have_v = 1; sscanf(line + 2, "video %*d %*s %d", &cur->pt); }
            else if (strncmp(line + 2, "audio", 5) == 0 && !have_a) { cur = audio; memset(cur, 0, sizeof(*cur)); cur->codec = BASIS_CODEC_AAC; cur->clock = 48000; cur->channels = 2; have_a = 1; sscanf(line + 2, "audio %*d %*s %d", &cur->pt); }
            else cur = NULL;
        } else if (cur && line[0] == 'a' && line[1] == '=') {
            const char* a = line + 2;
            if (strncmp(a, "rtpmap:", 7) == 0) {
                int pt = 0; char name[64] = {0}; int clk = 0, ch = 0;
                sscanf(a + 7, "%d %63[^/]/%d/%d", &pt, name, &clk, &ch);
                if (clk) cur->clock = clk;
                if (ch) cur->channels = ch;
                if (strncasecmp(name, "H265", 4) == 0 || strncasecmp(name, "HEVC", 4) == 0) cur->codec = BASIS_CODEC_H265;
                else if (strncasecmp(name, "H264", 4) == 0) cur->codec = BASIS_CODEC_H264;
                else if (strncasecmp(name, "mpeg4-generic", 13) == 0 || strncasecmp(name, "MP4A", 4) == 0) cur->codec = BASIS_CODEC_AAC;
            } else if (strncmp(a, "fmtp:", 5) == 0) {
                parse_fmtp(cur, a + 5);
            } else if (strncmp(a, "control:", 8) == 0) {
                strncpy(cur->control, a + 8, sizeof(cur->control) - 1);
            }
        }
    }
    return (have_v ? 1 : 0) + (have_a ? 1 : 0);
}

static void build_control_url(const char* base, const char* control, char* out, int outcap) {
    if (!control[0] || strcmp(control, "*") == 0) { snprintf(out, outcap, "%s", base); return; }
    if (strncmp(control, "rtsp://", 7) == 0) { snprintf(out, outcap, "%s", control); return; }
    if (control[0] == '/') snprintf(out, outcap, "%s%s", base, control);
    else snprintf(out, outcap, "%s/%s", base, control);
}

/* ---- RTP depacketization ----------------------------------------------- */

typedef struct {
    basis_media_sink_t* sink;
    sdp_media_t* video;
    sdp_media_t* audio;
    int v_channel, a_channel;

    uint8_t* au; int au_len, au_cap;     /* current video access unit (Annex B) */
    int64_t au_ts;                       /* RTP ts of current AU */
    int have_au_ts;
    int video_announced;
    int audio_announced;

    uint8_t* fu; int fu_len, fu_cap;     /* FU reassembly */
    int fu_active;
    uint8_t fu_nal_header0, fu_nal_header1; /* reconstructed NAL header (1 byte h264 / 2 bytes h265) */
    int fu_is_h265;
} depkt_t;

static const uint8_t SC4[4] = {0,0,0,1};

static int grow(uint8_t** b, int* cap, int need) {
    if (need <= *cap) return 1;
    int nc = *cap ? *cap * 2 : 65536;
    while (nc < need) nc *= 2;
    uint8_t* nb = (uint8_t*)realloc(*b, (size_t)nc);
    if (!nb) return 0;
    *b = nb; *cap = nc; return 1;
}

static void au_append_nal(depkt_t* d, const uint8_t* nal, int len) {
    if (!grow(&d->au, &d->au_cap, d->au_len + 4 + len)) return;
    memcpy(d->au + d->au_len, SC4, 4); d->au_len += 4;
    memcpy(d->au + d->au_len, nal, len); d->au_len += len;
}

static int64_t rtp_ts_to_us(int64_t ts, int clock) { return clock > 0 ? ts * 1000000 / clock : ts; }

static void deliver_au(depkt_t* d) {
    if (d->au_len <= 0) return;
    if (!d->video_announced) {
        int w = 0, h = 0;
        if (d->video->codec == BASIS_CODEC_H264 && d->video->extradata_len > 0) {
            int pos = 0, no, nl;
            while ((pos = basis_annexb_next(d->video->extradata, d->video->extradata_len, pos, &no, &nl)) >= 0)
                if (nl > 0 && basis_h264_nal_type(d->video->extradata[no]) == 7) { basis_h264_sps_dimensions(d->video->extradata + no, nl, &w, &h); break; }
        }
        d->sink->on_video_format(d->sink->user, d->video->codec, d->video->extradata, d->video->extradata_len, w, h);
        d->video_announced = 1;
    }
    int key = d->video->codec == BASIS_CODEC_H265 ? basis_h265_is_keyframe(d->au, d->au_len)
                                                  : basis_h264_is_keyframe(d->au, d->au_len);
    int64_t pts = rtp_ts_to_us(d->au_ts, d->video->clock);
    d->sink->on_video_au(d->sink->user, d->au, d->au_len, pts, pts, key);
    d->au_len = 0;
}

static void depkt_video(depkt_t* d, const uint8_t* rtp, int len) {
    if (len < 12) return;
    int cc = rtp[0] & 0x0F;
    int marker = (rtp[1] >> 7) & 1;
    uint32_t ts = (rtp[4] << 24) | (rtp[5] << 16) | (rtp[6] << 8) | rtp[7];
    int hdr = 12 + cc * 4;
    if ((rtp[0] & 0x10)) { /* extension */
        if (len < hdr + 4) return;
        int extlen = (rtp[hdr + 2] << 8) | rtp[hdr + 3];
        hdr += 4 + extlen * 4;
    }
    if (hdr >= len) return;
    const uint8_t* p = rtp + hdr;
    int plen = len - hdr;

    if (d->have_au_ts && (int64_t)ts != d->au_ts) { deliver_au(d); }
    d->au_ts = (int64_t)ts; d->have_au_ts = 1;

    int is_h265 = d->video->codec == BASIS_CODEC_H265;
    if (!is_h265) {
        int nt = p[0] & 0x1F;
        if (nt >= 1 && nt <= 23) {                 /* single NAL */
            au_append_nal(d, p, plen);
        } else if (nt == 24) {                      /* STAP-A */
            int i = 1;
            while (i + 2 <= plen) {
                int nsz = (p[i] << 8) | p[i + 1]; i += 2;
                if (i + nsz > plen) break;
                au_append_nal(d, p + i, nsz); i += nsz;
            }
        } else if (nt == 28) {                      /* FU-A */
            if (plen < 2) return;
            int s = (p[1] >> 7) & 1, e = (p[1] >> 6) & 1;
            int otype = p[1] & 0x1F;
            if (s) { d->fu_active = 1; d->fu_len = 0; d->fu_nal_header0 = (uint8_t)((p[0] & 0xE0) | otype); }
            if (d->fu_active && grow(&d->fu, &d->fu_cap, d->fu_len + (plen - 2))) {
                memcpy(d->fu + d->fu_len, p + 2, plen - 2); d->fu_len += plen - 2;
            }
            if (e && d->fu_active) {
                if (grow(&d->au, &d->au_cap, d->au_len + 4 + 1 + d->fu_len)) {
                    memcpy(d->au + d->au_len, SC4, 4); d->au_len += 4;
                    d->au[d->au_len++] = d->fu_nal_header0;
                    memcpy(d->au + d->au_len, d->fu, d->fu_len); d->au_len += d->fu_len;
                }
                d->fu_active = 0;
            }
        }
    } else {
        int nt = (p[0] >> 1) & 0x3F;
        if (nt <= 47) {                              /* single NAL (HEVC) */
            au_append_nal(d, p, plen);
        } else if (nt == 48) {                       /* AP (aggregation) */
            int i = 2;
            while (i + 2 <= plen) {
                int nsz = (p[i] << 8) | p[i + 1]; i += 2;
                if (i + nsz > plen) break;
                au_append_nal(d, p + i, nsz); i += nsz;
            }
        } else if (nt == 49) {                        /* FU (HEVC) */
            if (plen < 3) return;
            int s = (p[2] >> 7) & 1, e = (p[2] >> 6) & 1;
            int otype = p[2] & 0x3F;
            if (s) {
                d->fu_active = 1; d->fu_len = 0;
                d->fu_nal_header0 = (uint8_t)((p[0] & 0x81) | (otype << 1));
                d->fu_nal_header1 = p[1];
            }
            if (d->fu_active && grow(&d->fu, &d->fu_cap, d->fu_len + (plen - 3))) {
                memcpy(d->fu + d->fu_len, p + 3, plen - 3); d->fu_len += plen - 3;
            }
            if (e && d->fu_active) {
                if (grow(&d->au, &d->au_cap, d->au_len + 4 + 2 + d->fu_len)) {
                    memcpy(d->au + d->au_len, SC4, 4); d->au_len += 4;
                    d->au[d->au_len++] = d->fu_nal_header0;
                    d->au[d->au_len++] = d->fu_nal_header1;
                    memcpy(d->au + d->au_len, d->fu, d->fu_len); d->au_len += d->fu_len;
                }
                d->fu_active = 0;
            }
        }
    }

    if (marker) deliver_au(d);
}

static void depkt_audio(depkt_t* d, const uint8_t* rtp, int len) {
    if (len < 12 || !d->audio) return;
    int cc = rtp[0] & 0x0F;
    uint32_t ts = (rtp[4] << 24) | (rtp[5] << 16) | (rtp[6] << 8) | rtp[7];
    int hdr = 12 + cc * 4;
    if (hdr >= len) return;
    const uint8_t* p = rtp + hdr; int plen = len - hdr;

    /* MPEG4-GENERIC: 2-byte AU-headers-length (bits), then 16-bit AU-headers
     * (13-bit size + 3-bit index), then AU data. */
    if (plen < 2) return;
    int au_headers_bits = (p[0] << 8) | p[1];
    int num = au_headers_bits / 16;
    int dpos = 2 + num * 2;
    if (num <= 0 || dpos > plen) { /* assume single AU, no header */ num = 1; dpos = 0; }

    if (!d->audio_announced) {
        int sr = d->audio->clock ? d->audio->clock : 48000;
        int ch = d->audio->channels ? d->audio->channels : 2;
        d->sink->on_audio_format(d->sink->user, BASIS_CODEC_AAC, sr, ch,
                                 d->audio->asc_len ? d->audio->asc : NULL, d->audio->asc_len);
        d->audio_announced = 1;
    }

    int off = dpos;
    for (int i = 0; i < num; ++i) {
        int sz;
        if (dpos == 0) sz = plen; /* whole payload */
        else sz = ((p[2 + i * 2] << 8) | p[2 + i * 2 + 1]) >> 3;
        if (off + sz > plen) sz = plen - off;
        if (sz <= 0) break;
        int64_t pts = rtp_ts_to_us((int64_t)ts, d->audio->clock ? d->audio->clock : 48000);
        d->sink->on_audio_frame(d->sink->user, p + off, sz, pts);
        off += sz;
    }
}

/* ---- main run ----------------------------------------------------------- */

int basis_rtsp_run(basis_media_sink_t* sink, const basis_url_t* url) {
    rtsp_t r; memset(&r, 0, sizeof(r));
    char base_url[1024];
    snprintf(base_url, sizeof(base_url), "rtsp://%s:%d%s", url->host, url->port, url->path);

    if (url->user[0]) {
        char up[256]; int n = snprintf(up, sizeof(up), "%s:%s", url->user, url->pass);
        /* base64 of user:pass */
        static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        int o = 0;
        for (int i = 0; i < n; i += 3) {
            int b0 = (unsigned char)up[i];
            int b1 = i + 1 < n ? (unsigned char)up[i + 1] : 0;
            int b2 = i + 2 < n ? (unsigned char)up[i + 2] : 0;
            r.authb64[o++] = A[b0 >> 2];
            r.authb64[o++] = A[((b0 & 3) << 4) | (b1 >> 4)];
            r.authb64[o++] = i + 1 < n ? A[((b1 & 15) << 2) | (b2 >> 6)] : '=';
            r.authb64[o++] = i + 2 < n ? A[b2 & 63] : '=';
        }
        r.authb64[o] = 0;
    }

    r.io = basis_io_connect(url->host, url->port, 10000);
    if (!r.io) { sink->on_error(sink->user, "RTSP: TCP connect failed"); return -1; }

    char body[8192]; int blen = 0;

    /* Go straight to DESCRIBE. OPTIONS is only a capability probe — it costs a full
     * round trip on every connect and some servers drop the link right after it
     * (which is why DESCRIBE-first was already the recovery path). Auth is sent
     * pre-emptively on every request, so OPTIONS wasn't needed for discovery. */
    int desc_send = rtsp_send(&r, "DESCRIBE", base_url, "Accept: application/sdp\r\n");
    int code = (desc_send == 0) ? rtsp_recv(&r, body, sizeof(body) - 1, &blen) : -2;

    /* one reconnect retry if the first request didn't land (transient/half-open) */
    if (desc_send != 0 || code < 0) {
        basis_io_close(r.io);
        r.io = basis_io_connect(url->host, url->port, 10000);
        if (r.io) {
            basis_io_set_read_timeout(r.io, 10000);
            r.cseq = 0; r.session[0] = 0;
            desc_send = rtsp_send(&r, "DESCRIBE", base_url, "Accept: application/sdp\r\n");
            code = (desc_send == 0) ? rtsp_recv(&r, body, sizeof(body) - 1, &blen) : -2;
        }
    }

    if (code != 200 || blen <= 0) {
        char emsg[800];
        snprintf(emsg, sizeof(emsg),
                 "RTSP: DESCRIBE failed (desc_send=%d desc='%s' code=%d body=%dB auth='%s' loc='%s' url=%s)",
                 desc_send, r.last_status[0] ? r.last_status : "<none>", code, blen,
                 r.www_auth[0] ? r.www_auth : "none", r.location[0] ? r.location : "none", base_url);
        sink->on_error(sink->user, emsg);
        if (r.io) basis_io_close(r.io);
        return -1;
    }
    body[blen] = 0;

    sdp_media_t video, audio; int nmedia;
    memset(&video, 0, sizeof(video)); memset(&audio, 0, sizeof(audio));
    video.pt = audio.pt = -1; /* pt 0 is a valid payload type; -1 marks the media absent */
    nmedia = parse_sdp(body, blen, &video, &audio);
    if (nmedia == 0 || video.pt < 0) { sink->on_error(sink->user, "RTSP: no usable media in SDP"); basis_io_close(r.io); return -1; }

    /* SETUP video on interleaved channels 0-1 */
    int interleave = 0;
    int v_channel = -1, a_channel = -1;
    {
        char setup_url[1024], extra[128];
        build_control_url(base_url, video.control, setup_url, sizeof(setup_url));
        snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", interleave, interleave + 1);
        rtsp_send(&r, "SETUP", setup_url, extra);
        if (rtsp_recv(&r, NULL, 0, NULL) != 200) {
            char e[360]; snprintf(e, sizeof(e), "RTSP: SETUP video failed (status='%s' url=%s)", r.last_status, setup_url);
            sink->on_error(sink->user, e); basis_io_close(r.io); return -1;
        }
        v_channel = interleave; interleave += 2;
    }
    if (audio.pt >= 0) {
        char setup_url[1024], extra[128];
        build_control_url(base_url, audio.control, setup_url, sizeof(setup_url));
        snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n", interleave, interleave + 1);
        rtsp_send(&r, "SETUP", setup_url, extra);
        if (rtsp_recv(&r, NULL, 0, NULL) == 200) { a_channel = interleave; interleave += 2; }
    }

    rtsp_send(&r, "PLAY", base_url, "Range: npt=0.000-\r\n");
    if (rtsp_recv(&r, NULL, 0, NULL) != 200) {
        char e[320]; snprintf(e, sizeof(e), "RTSP: PLAY failed (status='%s')", r.last_status);
        sink->on_error(sink->user, e); basis_io_close(r.io); return -1;
    }

    basis_io_set_read_timeout(r.io, 10000);
    sink->on_state(sink->user, BASIS_MEDIA_STATE_BUFFERING);

    depkt_t d; memset(&d, 0, sizeof(d));
    d.sink = sink; d.video = &video; d.audio = audio.pt >= 0 ? &audio : NULL;
    d.v_channel = v_channel; d.a_channel = a_channel;

    /* interleaved frame: '$' <channel:1> <len:2> <RTP...> */
    uint8_t* pkt = NULL; int pkt_cap = 0;
    int rc = 0;
    while (sink->is_running(sink->user)) {
        uint8_t magic;
        if (basis_io_read_full(r.io, &magic, 1) != 1) { rc = -1; break; }
        if (magic != '$') {
            /* Could be an interim RTSP response (e.g., keepalive). Skip the line. */
            continue;
        }
        uint8_t hdr[3];
        if (basis_io_read_full(r.io, hdr, 3) != 3) { rc = -1; break; }
        int channel = hdr[0];
        int plen = (hdr[1] << 8) | hdr[2];
        if (plen <= 0 || plen > 4 * 1024 * 1024) { rc = -1; break; }
        if (!grow(&pkt, &pkt_cap, plen)) { rc = -1; break; }
        if (basis_io_read_full(r.io, pkt, plen) != plen) { rc = -1; break; }

        if (channel == d.v_channel) depkt_video(&d, pkt, plen);
        else if (channel == d.a_channel) depkt_audio(&d, pkt, plen);
        /* RTCP channels (odd) are ignored */
    }

    if (d.au_len > 0) deliver_au(&d);

    /* TEARDOWN best-effort */
    rtsp_send(&r, "TEARDOWN", base_url, NULL);

    free(pkt); free(d.au); free(d.fu);
    basis_io_close(r.io);
    return rc;
}
