#include "basis_caption.h"
#include "basis_bitstream.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
  #include <windows.h>
  typedef CRITICAL_SECTION bc_mutex_t;
  static void bc_mutex_init(bc_mutex_t* m)    { InitializeCriticalSection(m); }
  static void bc_mutex_destroy(bc_mutex_t* m) { DeleteCriticalSection(m); }
  static void bc_lock(bc_mutex_t* m)          { EnterCriticalSection(m); }
  static void bc_unlock(bc_mutex_t* m)        { LeaveCriticalSection(m); }
#else
  #include <pthread.h>
  typedef pthread_mutex_t bc_mutex_t;
  static void bc_mutex_init(bc_mutex_t* m)    { pthread_mutex_init(m, NULL); }
  static void bc_mutex_destroy(bc_mutex_t* m) { pthread_mutex_destroy(m); }
  static void bc_lock(bc_mutex_t* m)          { pthread_mutex_lock(m); }
  static void bc_unlock(bc_mutex_t* m)        { pthread_mutex_unlock(m); }
#endif

/* ---- CEA-608 character maps (to Unicode code points) -------------------- */

/* Basic North American set: 0x20-0x7F is ASCII bar these substitutions. */
static uint16_t basic_cp(uint8_t c) {
    switch (c) {
        case 0x2A: return 0x00E1; /* á */
        case 0x5C: return 0x00E9; /* é */
        case 0x5E: return 0x00ED; /* í */
        case 0x5F: return 0x00F3; /* ó */
        case 0x60: return 0x00FA; /* ú */
        case 0x7B: return 0x00E7; /* ç */
        case 0x7C: return 0x00F7; /* ÷ */
        case 0x7D: return 0x00D1; /* Ñ */
        case 0x7E: return 0x00F1; /* ñ */
        case 0x7F: return 0x2588; /* █ */
        default:   return c;
    }
}

/* Special characters: control 0x11, second byte 0x30-0x3F. */
static const uint16_t kSpecial[16] = {
    0x00AE, 0x00B0, 0x00BD, 0x00BF, 0x2122, 0x00A2, 0x00A3, 0x266A,
    0x00E0, 0x0020, 0x00E8, 0x00E2, 0x00EA, 0x00EE, 0x00F4, 0x00FB
};

/* Extended Spanish/Miscellaneous/French: control 0x12, second byte 0x20-0x3F. */
static const uint16_t kExt12[32] = {
    0x00C1, 0x00C9, 0x00D3, 0x00DA, 0x00DC, 0x00FC, 0x2018, 0x00A1,
    0x002A, 0x2019, 0x2014, 0x00A9, 0x2120, 0x2022, 0x201C, 0x201D,
    0x00C0, 0x00C2, 0x00C7, 0x00C8, 0x00CA, 0x00CB, 0x00EB, 0x00CE,
    0x00CF, 0x00EF, 0x00D4, 0x00D9, 0x00F9, 0x00DB, 0x00AB, 0x00BB
};

/* Extended Portuguese/German/Danish: control 0x13, second byte 0x20-0x3F. */
static const uint16_t kExt13[32] = {
    0x00C3, 0x00E3, 0x00CD, 0x00CC, 0x00EC, 0x00D2, 0x00F2, 0x00D5,
    0x00F5, 0x007B, 0x007D, 0x005C, 0x005E, 0x005F, 0x007C, 0x007E,
    0x00C4, 0x00E4, 0x00D6, 0x00F6, 0x00DF, 0x00A5, 0x00A4, 0x2502,
    0x00C5, 0x00E5, 0x00D8, 0x00F8, 0x250C, 0x2510, 0x2514, 0x2518
};

/* ---- CEA-608 decoder ---------------------------------------------------- */

#define ROWS 15
#define COLS 32

enum { MODE_POPON, MODE_ROLLUP, MODE_PAINTON };

typedef struct {
    uint16_t disp[ROWS][COLS];  /* displayed memory */
    uint16_t nond[ROWS][COLS];  /* non-displayed memory (pop-on load buffer) */
    int mode;
    int rollup;                 /* 2, 3 or 4 rows */
    int row, col;               /* cursor (0-based) */
    uint8_t last0, last1;       /* previous control pair, for doubling dedup */
    int have_last;
} cea608_t;

static void cea608_reset(cea608_t* s) {
    memset(s, 0, sizeof(*s));
    s->mode = MODE_POPON;
    s->rollup = 2;
    s->row = ROWS - 1;
}

/* The buffer characters land in: pop-on loads off-screen, the others draw live. */
static uint16_t (*cur_buf(cea608_t* s))[COLS] {
    return (s->mode == MODE_POPON) ? s->nond : s->disp;
}

/* Pop-on loads into off-screen memory (no visible change until EOC flips it);
 * roll-up and paint-on write straight to displayed memory, so each write is live. */
static int live(const cea608_t* s) { return s->mode != MODE_POPON; }

static void put_cp(cea608_t* s, uint16_t cp) {
    if (s->row < 0 || s->row >= ROWS) return;
    if (s->col < 0) s->col = 0;
    if (s->col >= COLS) return;
    cur_buf(s)[s->row][s->col] = cp ? cp : 0x20;
    s->col++;
}

/* Extended chars are transmitted after a standard fallback char and overwrite it. */
static void put_ext(cea608_t* s, uint16_t cp) {
    if (s->col > 0) s->col--;
    put_cp(s, cp);
}

/* PAC row from the control pair (1-based); see CEA-608 §8.4. */
static int pac_row(uint8_t b0) {
    switch (b0 & 0x07) {
        case 0: return 11; /* 0x10 */
        case 1: return 1;  /* 0x11 */
        case 2: return 3;  /* 0x12 */
        case 3: return 12; /* 0x13 */
        case 4: return 14; /* 0x14 */
        case 5: return 5;  /* 0x15 */
        case 6: return 7;  /* 0x16 */
        default: return 9; /* 0x17 */
    }
}

static void apply_pac(cea608_t* s, uint8_t b0, uint8_t b1) {
    int row = pac_row(b0);
    if (b0 != 0x10 && b1 >= 0x60) row += 1;
    s->row = row - 1;
    /* Indent PACs (bit 4 set) carry a column; colour/style PACs leave column 0. */
    s->col = (b1 & 0x10) ? (((b1 & 0x0E) >> 1) * 4) : 0;
    if (s->col >= COLS) s->col = 0;
}

static void rollup_scroll(cea608_t* s) {
    int base = (s->row >= 0 && s->row < ROWS) ? s->row : ROWS - 1;
    int rows = s->rollup < 2 ? 2 : s->rollup;
    int top = base - (rows - 1);
    if (top < 0) top = 0;
    for (int r = top; r < base; ++r)
        memcpy(s->disp[r], s->disp[r + 1], sizeof(s->disp[r]));
    memset(s->disp[base], 0, sizeof(s->disp[base]));
    s->row = base;
    s->col = 0;
}

/* Misc control (control 0x14, second byte 0x20-0x2F). Returns 1 if displayed
 * memory changed and a cue should be emitted. */
static int misc_control(cea608_t* s, uint8_t b1) {
    uint16_t (*buf)[COLS] = cur_buf(s);
    switch (b1) {
        case 0x20: s->mode = MODE_POPON; return 0;                    /* RCL */
        case 0x21: if (s->col > 0) { s->col--; buf[s->row][s->col] = 0; } return live(s); /* BS */
        case 0x24: for (int c = s->col; c < COLS; ++c) buf[s->row][c] = 0; return live(s); /* DER */
        case 0x25: s->mode = MODE_ROLLUP; s->rollup = 2; return 0;    /* RU2 */
        case 0x26: s->mode = MODE_ROLLUP; s->rollup = 3; return 0;    /* RU3 */
        case 0x27: s->mode = MODE_ROLLUP; s->rollup = 4; return 0;    /* RU4 */
        case 0x28: return 0;                                          /* FON */
        case 0x29: s->mode = MODE_PAINTON; return 0;                  /* RDC */
        case 0x2A: return 0;                                          /* TR  */
        case 0x2B: return 0;                                          /* RTD */
        case 0x2C: memset(s->disp, 0, sizeof(s->disp)); return 1;     /* EDM */
        case 0x2D: if (s->mode == MODE_ROLLUP) { rollup_scroll(s); return 1; } return 0; /* CR */
        case 0x2E: memset(s->nond, 0, sizeof(s->nond)); return 0;     /* ENM */
        case 0x2F: {                                                  /* EOC */
            uint16_t tmp[ROWS][COLS];
            memcpy(tmp, s->disp, sizeof(tmp));
            memcpy(s->disp, s->nond, sizeof(s->disp));
            memcpy(s->nond, tmp, sizeof(s->nond));
            return 1;
        }
        default: return 0;
    }
}

/* Decode one parity-stripped byte pair from field 1. Returns 1 when displayed
 * memory changed (the caller should serialise + emit a cue). */
static int cea608_pair(cea608_t* s, uint8_t b0, uint8_t b1) {
    b0 &= 0x7F; b1 &= 0x7F;
    if (b0 == 0 && b1 == 0) return 0;

    int is_ctrl = (b0 >= 0x10 && b0 <= 0x1F);
    if (is_ctrl) {
        if (s->have_last && b0 == s->last0 && b1 == s->last1) { s->have_last = 0; return 0; }
        s->last0 = b0; s->last1 = b1; s->have_last = 1;
    } else {
        s->have_last = 0;
    }

    if (b0 >= 0x18 && b0 <= 0x1F) return 0; /* channel 2 — slice 1 decodes CC1 only */

    if (is_ctrl) {
        if (b1 >= 0x40)                              { apply_pac(s, b0, b1); return 0; }
        if (b0 == 0x11 && b1 >= 0x20 && b1 <= 0x2F)  { put_cp(s, 0x20); return live(s); }      /* mid-row style */
        if (b0 == 0x11 && b1 >= 0x30 && b1 <= 0x3F)  { put_cp(s, kSpecial[b1 - 0x30]); return live(s); }
        if (b0 == 0x12 && b1 >= 0x20 && b1 <= 0x3F)  { put_ext(s, kExt12[b1 - 0x20]); return live(s); }
        if (b0 == 0x13 && b1 >= 0x20 && b1 <= 0x3F)  { put_ext(s, kExt13[b1 - 0x20]); return live(s); }
        if (b0 == 0x17 && b1 >= 0x21 && b1 <= 0x23)  { s->col += (b1 - 0x20); if (s->col > COLS) s->col = COLS; return 0; }
        if (b0 == 0x14 && b1 >= 0x20 && b1 <= 0x2F)  { return misc_control(s, b1); }
        return 0;
    }

    int wrote = 0;
    if (b0 >= 0x20) { put_cp(s, basic_cp(b0)); wrote = 1; }
    if (b1 >= 0x20) { put_cp(s, basic_cp(b1)); wrote = 1; }
    return wrote ? live(s) : 0;
}

static int utf8_put(char* out, int cap, int n, uint16_t cp) {
    if (cp < 0x80) {
        if (n + 1 > cap) return n;
        out[n++] = (char)cp;
    } else if (cp < 0x800) {
        if (n + 2 > cap) return n;
        out[n++] = (char)(0xC0 | (cp >> 6));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (n + 3 > cap) return n;
        out[n++] = (char)(0xE0 | (cp >> 12));
        out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[n++] = (char)(0x80 | (cp & 0x3F));
    }
    return n;
}

/* Flatten displayed memory to UTF-8: non-empty rows top-to-bottom, leading and
 * trailing spaces trimmed, rows joined with '\n'. */
static void cea608_serialize(const cea608_t* s, char* out, int cap) {
    int n = 0, first = 1;
    for (int r = 0; r < ROWS; ++r) {
        int l = 0, rr = COLS - 1;
        while (l < COLS && s->disp[r][l] == 0) l++;
        while (rr >= 0 && s->disp[r][rr] == 0) rr--;
        while (l <= rr && s->disp[r][l] == 0x20) l++;
        while (rr >= l && s->disp[r][rr] == 0x20) rr--;
        if (l > rr) continue;
        if (!first) n = utf8_put(out, cap - 1, n, '\n');
        first = 0;
        for (int c = l; c <= rr; ++c)
            n = utf8_put(out, cap - 1, n, s->disp[r][c] ? s->disp[r][c] : 0x20);
    }
    out[n] = 0;
}

/* ---- cue store ---------------------------------------------------------- */

#define CUE_TEXT_MAX 256
#define CUE_RING 64

/* PTS gap (µs) beyond which a backwards jump is treated as a new timeline rather
 * than B-frame decode-order reordering (which is sub-second). */
#define CAPTION_EPOCH_SLACK_US 1000000

typedef struct {
    int64_t start, end;
    char text[CUE_TEXT_MAX];
} cue_t;

struct basis_caption_ctx {
    bc_mutex_t lock;     /* guards the cue ring (poll vs demux thread) */
    cea608_t dec;        /* demux-thread only — not under lock */
    cue_t ring[CUE_RING];
    int head;            /* next write slot */
    int count;
    int64_t last_pts;    /* last scanned AU PTS; INT64_MIN until first AU */
};

/* Append a cue starting at pts_us; close the previous cue at the same instant.
 * An empty text marks a clear (poll returns no active cue). */
static void push_cue(basis_caption_ctx_t* c, int64_t pts_us, const char* text) {
    bc_lock(&c->lock);
    if (c->count > 0) {
        cue_t* prev = &c->ring[(c->head + CUE_RING - 1) % CUE_RING];
        if (pts_us > prev->start) prev->end = pts_us;
    }
    cue_t* cue = &c->ring[c->head];
    cue->start = pts_us;
    cue->end = INT64_MAX;
    size_t len = strlen(text);
    if (len >= CUE_TEXT_MAX) len = CUE_TEXT_MAX - 1;
    memcpy(cue->text, text, len);
    cue->text[len] = 0;
    c->head = (c->head + 1) % CUE_RING;
    if (c->count < CUE_RING) c->count++;
    bc_unlock(&c->lock);
}

static void emit(basis_caption_ctx_t* c, int64_t pts_us) {
    char text[CUE_TEXT_MAX];
    cea608_serialize(&c->dec, text, sizeof(text));
    push_cue(c, pts_us, text);
}

/* ---- SEI extraction ----------------------------------------------------- */

/* Parse the ATSC A/53 cc_data() payload of a registered user-data SEI message
 * and feed the field-1 pairs into the 608 decoder. */
static void parse_user_data(basis_caption_ctx_t* c, const uint8_t* d, int len, int64_t pts_us) {
    if (len < 8) return;
    if (d[0] != 0xB5) return;                       /* itu_t_t35_country_code = USA */
    if (((d[1] << 8) | d[2]) != 0x0031) return;     /* provider_code = ATSC */
    if (memcmp(d + 3, "GA94", 4) != 0) return;      /* user_identifier */
    if (d[7] != 0x03) return;                       /* user_data_type_code = cc_data */

    const uint8_t* cc = d + 8;
    int cclen = len - 8;
    if (cclen < 2) return;
    if (!((cc[0] >> 6) & 1)) return;                /* process_cc_data_flag */
    int count = cc[0] & 0x1F;
    int idx = 2;                                    /* skip flags byte + em_data byte */
    int changed = 0;
    for (int i = 0; i < count; ++i) {
        if (idx + 3 > cclen) break;
        uint8_t f = cc[idx];
        int valid = (f >> 2) & 1;
        int ctype = f & 0x3;
        uint8_t d1 = cc[idx + 1], d2 = cc[idx + 2];
        idx += 3;
        if (!valid) continue;
        if (ctype == 0)                             /* CEA-608 field 1 */
            changed |= cea608_pair(&c->dec, d1, d2);
        /* ctype 1 = field 2, 2/3 = CEA-708 DTVCC — not decoded in slice 1 */
    }
    /* One cue per AU: roll-up/paint-on mutate displayed memory on every pair, so
     * coalescing keeps the ring from churning while still tracking live updates. */
    if (changed) emit(c, pts_us);
}

/* Walk the SEI messages in one NAL (header stripped, RBSP unescaped). */
static void scan_sei_rbsp(basis_caption_ctx_t* c, const uint8_t* rbsp, int rlen, int64_t pts_us) {
    int p = 0;
    while (p + 2 <= rlen) {
        int type = 0;
        while (p < rlen && rbsp[p] == 0xFF) { type += 255; p++; }
        if (p >= rlen) break;
        type += rbsp[p++];
        int size = 0;
        while (p < rlen && rbsp[p] == 0xFF) { size += 255; p++; }
        if (p >= rlen) break;
        size += rbsp[p++];
        if (size < 0 || p + size > rlen) break;
        if (type == 4) parse_user_data(c, rbsp + p, size, pts_us);
        p += size;
        if (p < rlen && rbsp[p] == 0x80) break;     /* rbsp_trailing_bits */
    }
}

static void scan_nal_sei(basis_caption_ctx_t* c, const uint8_t* nal, int nal_len, int hevc, int64_t pts_us) {
    int hdr = hevc ? 2 : 1;
    if (nal_len <= hdr) return;
    /* Unescape emulation-prevention bytes (00 00 03). The common caption SEI is
     * tiny, so the stack buffer covers it; a larger NAL (caption payload trailing
     * other SEI messages) spills to the heap rather than truncating. */
    int cap = nal_len - hdr;            /* unescaping only shrinks the payload */
    uint8_t stackbuf[2048];
    uint8_t* rbsp = stackbuf;
    uint8_t* heap = NULL;
    if (cap > (int)sizeof(stackbuf)) {
        heap = (uint8_t*)malloc((size_t)cap);
        if (!heap) return;             /* skip the NAL rather than parse a partial one */
        rbsp = heap;
    }
    int rlen = 0, zeros = 0;
    for (int i = hdr; i < nal_len; ++i) {
        uint8_t b = nal[i];
        if (zeros >= 2 && b == 0x03) { zeros = 0; continue; }
        rbsp[rlen++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    scan_sei_rbsp(c, rbsp, rlen, pts_us);
    free(heap);
}

/* ---- public API --------------------------------------------------------- */

basis_caption_ctx_t* basis_caption_create(void) {
    basis_caption_ctx_t* c = (basis_caption_ctx_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    bc_mutex_init(&c->lock);
    cea608_reset(&c->dec);
    c->last_pts = INT64_MIN;
    return c;
}

void basis_caption_destroy(basis_caption_ctx_t* c) {
    if (!c) return;
    bc_mutex_destroy(&c->lock);
    free(c);
}

void basis_caption_scan_au(basis_caption_ctx_t* c, const uint8_t* annexb, int len,
                           int hevc, int64_t pts_us) {
    if (!c || !annexb || len <= 0) return;

    /* A large backwards PTS jump marks a new timeline (loop replay, reconnect or a
     * mid-stream discontinuity). Drop the decoder + cue ring so captions from the old
     * epoch can't outlive it. The slack absorbs B-frame decode-order reordering,
     * which is sub-second, without swallowing a real reset. */
    if (pts_us >= 0) {
        if (c->last_pts != INT64_MIN && pts_us + CAPTION_EPOCH_SLACK_US < c->last_pts) {
            bc_lock(&c->lock);
            c->head = 0;
            c->count = 0;
            bc_unlock(&c->lock);
            cea608_reset(&c->dec);
        }
        c->last_pts = pts_us;
    }

    int pos = 0, off, nl;
    while ((pos = basis_annexb_next(annexb, len, pos, &off, &nl)) >= 0) {
        if (nl <= 0) continue;
        uint8_t b0 = annexb[off];
        int t = hevc ? basis_h265_nal_type(b0) : basis_h264_nal_type(b0);
        int is_sei = hevc ? (t == 39 || t == 40) : (t == 6);
        if (is_sei) scan_nal_sei(c, annexb + off, nl, hevc, pts_us);
    }
}

int basis_caption_poll(basis_caption_ctx_t* c, int64_t presentation_pts_us,
                       char* buf, int buf_size,
                       int64_t* out_start_us, int64_t* out_end_us) {
    if (!c || !buf || buf_size <= 0) return -1;
    buf[0] = 0;
    if (presentation_pts_us < 0) return 0;

    int n = 0;
    bc_lock(&c->lock);
    for (int i = 0; i < c->count; ++i) {
        cue_t* cue = &c->ring[(c->head + CUE_RING - 1 - i) % CUE_RING];
        if (cue->start <= presentation_pts_us) {     /* newest cue at/after which we sit */
            if (out_start_us) *out_start_us = cue->start;
            if (out_end_us) *out_end_us = cue->end;
            if (cue->text[0]) {                      /* empty text = a clear cue */
                n = (int)strlen(cue->text);
                if (n >= buf_size) n = buf_size - 1;
                memcpy(buf, cue->text, (size_t)n);
            }
            break;
        }
    }
    bc_unlock(&c->lock);
    buf[n] = 0;
    return n;
}
