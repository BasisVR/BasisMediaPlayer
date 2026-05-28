/*
 * basis_jni_https.h — Android HTTPS byte source backed by java.net.HttpsURLConnection.
 *
 * AMediaExtractor handles the easy case (seekable container files over HTTPS).
 * Live MPEG-TS streams are rejected by AMediaExtractor with UNSUPPORTED, so the
 * core falls back to the portable basis_ts demuxer — and needs an HTTPS byte
 * source the demuxer can read from. NDK has no TLS API of its own; the smallest
 * dependency-free option is to bridge to Java via JNI.
 *
 * Same contract as protocol/basis_http: open returns an opaque ctx, read fills a
 * caller buffer (bytes read; 0 = EOF; <0 = error), close frees it. Read is called
 * on the demux thread; the JNI layer attaches that thread to the JVM as needed.
 */

#ifndef BASIS_JNI_HTTPS_H
#define BASIS_JNI_HTTPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void* basis_jni_https_open(const char* url, int timeout_ms);
int   basis_jni_https_read(void* ctx, uint8_t* buf, int len);
void  basis_jni_https_close(void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* BASIS_JNI_HTTPS_H */
