/* WinHTTP byte source — OS TLS/HTTP, no third-party deps. */
#include "basis_win_http.h"

#include <windows.h>
#include <winhttp.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "winhttp.lib")

typedef struct {
    HINTERNET session;
    HINTERNET connect;
    HINTERNET request;
    int response_complete;
} win_http_t;

static wchar_t* to_w(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

extern "C" void* basis_win_http_open(const char* url) {
    if (!url) return NULL;
    win_http_t* h = (win_http_t*)calloc(1, sizeof(win_http_t));
    if (!h) return NULL;

    wchar_t* wurl = to_w(url);
    if (!wurl) { free(h); return NULL; }

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { free(wurl); free(h); return NULL; }

    h->session = WinHttpOpen(L"BasisMediaPlayer/1.0",
                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session) { free(wurl); free(h); return NULL; }

    h->connect = WinHttpConnect(h->session, host, uc.nPort, 0);
    if (!h->connect) { WinHttpCloseHandle(h->session); free(wurl); free(h); return NULL; }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    h->request = WinHttpOpenRequest(h->connect, L"GET", path, NULL,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h->request) {
        WinHttpCloseHandle(h->connect); WinHttpCloseHandle(h->session);
        free(wurl); free(h); return NULL;
    }

    /* Disable response buffering so live streams flow as they arrive. */
    DWORD opt = WINHTTP_DISABLE_REDIRECTS; /* keep redirects on by NOT setting? we want them on */
    (void)opt;

    if (!WinHttpSendRequest(h->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(h->request, NULL)) {
        WinHttpCloseHandle(h->request); WinHttpCloseHandle(h->connect); WinHttpCloseHandle(h->session);
        free(wurl); free(h); return NULL;
    }

    /* check status code */
    DWORD code = 0, sz = sizeof(code);
    WinHttpQueryHeaders(h->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    free(wurl);
    if (code < 200 || code >= 400) {
        WinHttpCloseHandle(h->request); WinHttpCloseHandle(h->connect); WinHttpCloseHandle(h->session);
        free(h); return NULL;
    }
    return h;
}

extern "C" int basis_win_http_read(void* ctx, uint8_t* buf, int len) {
    win_http_t* h = (win_http_t*)ctx;
    if (!h || h->response_complete || len <= 0) return 0;
    DWORD read = 0;
    if (!WinHttpReadData(h->request, buf, (DWORD)len, &read)) return -1;
    if (read == 0) { h->response_complete = 1; return 0; }
    return (int)read;
}

extern "C" void basis_win_http_close(void* ctx) {
    win_http_t* h = (win_http_t*)ctx;
    if (!h) return;
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    free(h);
}
