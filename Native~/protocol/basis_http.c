#include "basis_http.h"
#include "basis_io.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
    basis_io_t* io;
    int chunked;
    int has_length;
    long long remaining;   /* content-length remaining, or current chunk remaining */
    int eof;

    /* leftover bytes already pulled from the socket past the header boundary */
    uint8_t lead[8192];
    int lead_len;
    int lead_pos;
} http_ctx;

static int lead_take(http_ctx* h, uint8_t* dst, int len) {
    int avail = h->lead_len - h->lead_pos;
    if (avail <= 0) return 0;
    int n = len < avail ? len : avail;
    memcpy(dst, h->lead + h->lead_pos, (size_t)n);
    h->lead_pos += n;
    return n;
}

/* read one line (up to CRLF) using leftover + socket; returns line length w/o CRLF */
static int read_line(http_ctx* h, char* out, int cap) {
    int n = 0;
    while (n < cap - 1) {
        uint8_t c;
        int got = lead_take(h, &c, 1);
        if (got == 0) got = basis_io_read(h->io, &c, 1);
        if (got <= 0) return n > 0 ? n : -1;
        if (c == '\n') break;
        if (c != '\r') out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

void* basis_http_open(const basis_url_t* url, int timeout_ms) {
    if (!url) return NULL;
    http_ctx* h = (http_ctx*)calloc(1, sizeof(*h));
    if (!h) return NULL;

    h->io = basis_io_connect(url->host, url->port, timeout_ms);
    if (!h->io) { free(h); return NULL; }

    char req[2048];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BasisMediaPlayer/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        url->path[0] ? url->path : "/", url->host);
    if (basis_io_write_full(h->io, (const uint8_t*)req, rl) != rl) {
        basis_io_close(h->io); free(h); return NULL;
    }

    /* status line */
    char line[1024];
    if (read_line(h, line, sizeof(line)) < 0) { basis_io_close(h->io); free(h); return NULL; }
    int code = 0;
    { const char* sp = strchr(line, ' '); if (sp) code = atoi(sp + 1); }
    if (code < 200 || code >= 400) { basis_io_close(h->io); free(h); return NULL; }

    /* headers */
    h->has_length = 0; h->chunked = 0; h->remaining = -1;
    for (;;) {
        int ll = read_line(h, line, sizeof(line));
        if (ll <= 0) break; /* blank line ends headers (ll==0) or error (<0) */
        /* lowercase the header name for comparison */
        char low[256]; int i = 0;
        for (; line[i] && line[i] != ':' && i < (int)sizeof(low) - 1; ++i) low[i] = (char)tolower((unsigned char)line[i]);
        low[i] = 0;
        const char* val = strchr(line, ':');
        if (val) { val++; while (*val == ' ') val++; }
        if (strcmp(low, "content-length") == 0 && val) { h->has_length = 1; h->remaining = atoll(val); }
        else if (strcmp(low, "transfer-encoding") == 0 && val && strstr(val, "chunked")) { h->chunked = 1; }
    }

    /* For chunked, remaining starts at 0 meaning "read next chunk size". */
    if (h->chunked) h->remaining = 0;
    return h;
}

/* read the next chunk-size line for chunked encoding */
static int next_chunk(http_ctx* h) {
    char line[64];
    /* a CRLF trails each chunk's data; consume any stray leading CRLF */
    int ll = read_line(h, line, sizeof(line));
    if (ll < 0) return -1;
    if (ll == 0) { /* trailing CRLF after previous chunk -> read the size line */
        ll = read_line(h, line, sizeof(line));
        if (ll < 0) return -1;
    }
    long sz = strtol(line, NULL, 16);
    if (sz <= 0) { h->eof = 1; return 0; }
    h->remaining = sz;
    return 1;
}

int basis_http_read(void* ctx, uint8_t* buf, int len) {
    http_ctx* h = (http_ctx*)ctx;
    if (!h || h->eof || len <= 0) return 0;

    if (h->chunked) {
        if (h->remaining <= 0) {
            int r = next_chunk(h);
            if (r <= 0) return r; /* 0 = EOF, -1 = error */
        }
        int want = (long long)len < h->remaining ? len : (int)h->remaining;
        int got = lead_take(h, buf, want);
        if (got == 0) got = basis_io_read(h->io, buf, want);
        if (got <= 0) { h->eof = 1; return got; }
        h->remaining -= got;
        return got;
    }

    if (h->has_length) {
        if (h->remaining <= 0) { h->eof = 1; return 0; }
        int want = (long long)len < h->remaining ? len : (int)h->remaining;
        int got = lead_take(h, buf, want);
        if (got == 0) got = basis_io_read(h->io, buf, want);
        if (got <= 0) { h->eof = 1; return got; }
        h->remaining -= got;
        return got;
    }

    /* no length, not chunked: stream until the server closes */
    int got = lead_take(h, buf, len);
    if (got == 0) got = basis_io_read(h->io, buf, len);
    if (got <= 0) { h->eof = 1; return got; }
    return got;
}

void basis_http_close(void* ctx) {
    http_ctx* h = (http_ctx*)ctx;
    if (!h) return;
    if (h->io) basis_io_close(h->io);
    free(h);
}
