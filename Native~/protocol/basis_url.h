/* Minimal absolute-URL parser for the media schemes we accept. */
#ifndef BASIS_URL_H
#define BASIS_URL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct basis_url {
    char scheme[16];   /* lowercased: rtsp, rtspt, rtmp, rtmps, http, https, rist */
    char host[256];
    int  port;         /* defaulted per scheme when absent */
    char path[1024];   /* everything after the host, including leading '/' and query */
    char user[128];
    char pass[128];
    int  tls;          /* 1 for rtmps/https */
} basis_url_t;

/* Returns 0 on success. Fills defaults: rtsp/rtspt=554, rtmp=1935, rtmps=443,
 * http=80, https=443. rtspt is normalised to scheme "rtsp" with tls=0 (the
 * "t" only means "interleave RTP over the TCP control channel"). */
int basis_url_parse(const char* url, basis_url_t* out);

/* 1 if the scheme uses our custom TCP control protocols (rtsp/rtmp family). */
int basis_url_is_rtsp(const basis_url_t* u);
int basis_url_is_rtmp(const basis_url_t* u);
int basis_url_is_rist(const basis_url_t* u);

#ifdef __cplusplus
}
#endif
#endif
