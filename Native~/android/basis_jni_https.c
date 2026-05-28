/*
 * basis_jni_https.c — bridges to java.net.URL/HttpsURLConnection so the portable
 * MPEG-TS / fMP4 demuxers can read HTTPS streams on Android. See header for the
 * contract and why this exists.
 *
 * Method/class IDs are cached at JNI_OnLoad (the only place the harness reliably
 * gives us the *system* class loader — FindClass from arbitrary threads can hit
 * the calling thread's class loader and miss app classes; for java.net types it
 * works either way, but caching avoids the per-read FindClass cost).
 */

#include "basis_jni_https.h"

#include <jni.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LOG_TAG "basis_media"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

static JavaVM* g_jvm = NULL;

static struct {
    jclass  url_cls;            /* java/net/URL                                  */
    jmethodID url_ctor;         /* URL(String)                                   */
    jmethodID url_open;         /* openConnection() -> URLConnection             */

    jclass  conn_cls;           /* java/net/URLConnection                        */
    jmethodID conn_set_ct;      /* setConnectTimeout(int)                        */
    jmethodID conn_set_rt;      /* setReadTimeout(int)                           */
    jmethodID conn_set_req;     /* setRequestProperty(String, String)            */
    jmethodID conn_connect;     /* connect()                                     */
    jmethodID conn_get_is;      /* getInputStream() -> InputStream               */

    jclass  http_conn_cls;      /* java/net/HttpURLConnection                    */
    jmethodID http_set_follow;  /* setInstanceFollowRedirects(boolean)           */
    jmethodID http_get_code;    /* getResponseCode() -> int                      */
    jmethodID http_disconnect;  /* disconnect()                                  */

    jclass  is_cls;             /* java/io/InputStream                           */
    jmethodID is_read;          /* read(byte[], int, int) -> int                 */
    jmethodID is_close;         /* close()                                       */
} g_ids;

static int g_init_ok = 0;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;

    JNIEnv* env = NULL;
    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    jclass url        = (*env)->FindClass(env, "java/net/URL");
    jclass conn       = (*env)->FindClass(env, "java/net/URLConnection");
    jclass httpconn   = (*env)->FindClass(env, "java/net/HttpURLConnection");
    jclass is         = (*env)->FindClass(env, "java/io/InputStream");
    if (!url || !conn || !httpconn || !is) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        return JNI_VERSION_1_6;
    }

    g_ids.url_cls       = (jclass)(*env)->NewGlobalRef(env, url);
    g_ids.conn_cls      = (jclass)(*env)->NewGlobalRef(env, conn);
    g_ids.http_conn_cls = (jclass)(*env)->NewGlobalRef(env, httpconn);
    g_ids.is_cls        = (jclass)(*env)->NewGlobalRef(env, is);

    g_ids.url_ctor       = (*env)->GetMethodID(env, g_ids.url_cls, "<init>", "(Ljava/lang/String;)V");
    g_ids.url_open       = (*env)->GetMethodID(env, g_ids.url_cls, "openConnection", "()Ljava/net/URLConnection;");
    g_ids.conn_set_ct    = (*env)->GetMethodID(env, g_ids.conn_cls, "setConnectTimeout", "(I)V");
    g_ids.conn_set_rt    = (*env)->GetMethodID(env, g_ids.conn_cls, "setReadTimeout", "(I)V");
    g_ids.conn_set_req   = (*env)->GetMethodID(env, g_ids.conn_cls, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_ids.conn_connect   = (*env)->GetMethodID(env, g_ids.conn_cls, "connect", "()V");
    g_ids.conn_get_is    = (*env)->GetMethodID(env, g_ids.conn_cls, "getInputStream", "()Ljava/io/InputStream;");
    g_ids.http_set_follow= (*env)->GetMethodID(env, g_ids.http_conn_cls, "setInstanceFollowRedirects", "(Z)V");
    g_ids.http_get_code  = (*env)->GetMethodID(env, g_ids.http_conn_cls, "getResponseCode", "()I");
    g_ids.http_disconnect= (*env)->GetMethodID(env, g_ids.http_conn_cls, "disconnect", "()V");
    g_ids.is_read        = (*env)->GetMethodID(env, g_ids.is_cls, "read", "([BII)I");
    g_ids.is_close       = (*env)->GetMethodID(env, g_ids.is_cls, "close", "()V");

    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return JNI_VERSION_1_6; /* leave g_init_ok == 0 — open() will refuse */
    }

    g_init_ok = 1;
    return JNI_VERSION_1_6;
}

/* ---- helpers ------------------------------------------------------------ */

typedef struct {
    JNIEnv* env;
    int     attached;
} jenv_lease;

static int jenv_acquire(jenv_lease* lease) {
    lease->env = NULL;
    lease->attached = 0;
    if (!g_jvm) return -1;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void**)&lease->env, JNI_VERSION_1_6);
    if (rc == JNI_OK) return 0;
    if (rc == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &lease->env, NULL) != JNI_OK) return -1;
        lease->attached = 1;
        return 0;
    }
    return -1;
}

static void jenv_release(jenv_lease* lease) {
    if (lease->attached && g_jvm) (*g_jvm)->DetachCurrentThread(g_jvm);
}

static void log_and_clear_pending(JNIEnv* env, const char* where) {
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        LOGE("basis_jni_https: %s: java exception (cleared)", where);
    }
}

/* ---- context ------------------------------------------------------------ */

typedef struct {
    jobject conn;       /* global ref: HttpURLConnection                    */
    jobject is;         /* global ref: InputStream                          */
    jbyteArray scratch; /* global ref: reusable byte[scratch_cap]           */
    int scratch_cap;
    int eof;
    long long total_bytes;
} https_ctx;

void* basis_jni_https_open(const char* url, int timeout_ms) {
    if (!url) return NULL;
    if (!g_init_ok) { LOGE("basis_jni_https: JNI not initialised"); return NULL; }

    jenv_lease L; if (jenv_acquire(&L) != 0) return NULL;
    JNIEnv* env = L.env;

    https_ctx* h = (https_ctx*)calloc(1, sizeof(*h));
    if (!h) { jenv_release(&L); return NULL; }

    jstring jurl = (*env)->NewStringUTF(env, url);
    jobject urlObj = (*env)->NewObject(env, g_ids.url_cls, g_ids.url_ctor, jurl);
    if ((*env)->ExceptionCheck(env) || !urlObj) {
        log_and_clear_pending(env, "new URL");
        (*env)->DeleteLocalRef(env, jurl);
        free(h); jenv_release(&L); return NULL;
    }

    jobject conn = (*env)->CallObjectMethod(env, urlObj, g_ids.url_open);
    if ((*env)->ExceptionCheck(env) || !conn) {
        log_and_clear_pending(env, "openConnection");
        (*env)->DeleteLocalRef(env, urlObj);
        (*env)->DeleteLocalRef(env, jurl);
        free(h); jenv_release(&L); return NULL;
    }

    if (timeout_ms > 0) {
        (*env)->CallVoidMethod(env, conn, g_ids.conn_set_ct, (jint)timeout_ms);
        (*env)->CallVoidMethod(env, conn, g_ids.conn_set_rt, (jint)timeout_ms);
    }
    jstring agent_key = (*env)->NewStringUTF(env, "User-Agent");
    jstring agent_val = (*env)->NewStringUTF(env, "BasisMediaPlayer/1.0");
    (*env)->CallVoidMethod(env, conn, g_ids.conn_set_req, agent_key, agent_val);
    (*env)->DeleteLocalRef(env, agent_key);
    (*env)->DeleteLocalRef(env, agent_val);

    /* HttpURLConnection (and its HttpsURLConnection subclass) gets redirect + status APIs. */
    if ((*env)->IsInstanceOf(env, conn, g_ids.http_conn_cls)) {
        (*env)->CallVoidMethod(env, conn, g_ids.http_set_follow, JNI_TRUE);
    }

    (*env)->CallVoidMethod(env, conn, g_ids.conn_connect);
    if ((*env)->ExceptionCheck(env)) {
        log_and_clear_pending(env, "connect");
        (*env)->DeleteLocalRef(env, conn);
        (*env)->DeleteLocalRef(env, urlObj);
        (*env)->DeleteLocalRef(env, jurl);
        free(h); jenv_release(&L); return NULL;
    }

    if ((*env)->IsInstanceOf(env, conn, g_ids.http_conn_cls)) {
        jint code = (*env)->CallIntMethod(env, conn, g_ids.http_get_code);
        if ((*env)->ExceptionCheck(env)) { log_and_clear_pending(env, "getResponseCode"); code = 0; }
        if (code < 200 || code >= 400) {
            LOGE("basis_jni_https: HTTP %d for %s", (int)code, url);
            (*env)->CallVoidMethod(env, conn, g_ids.http_disconnect);
            log_and_clear_pending(env, "disconnect");
            (*env)->DeleteLocalRef(env, conn);
            (*env)->DeleteLocalRef(env, urlObj);
            (*env)->DeleteLocalRef(env, jurl);
            free(h); jenv_release(&L); return NULL;
        }
    }

    jobject is = (*env)->CallObjectMethod(env, conn, g_ids.conn_get_is);
    if ((*env)->ExceptionCheck(env) || !is) {
        log_and_clear_pending(env, "getInputStream");
        if ((*env)->IsInstanceOf(env, conn, g_ids.http_conn_cls))
            (*env)->CallVoidMethod(env, conn, g_ids.http_disconnect);
        log_and_clear_pending(env, "disconnect");
        (*env)->DeleteLocalRef(env, conn);
        (*env)->DeleteLocalRef(env, urlObj);
        (*env)->DeleteLocalRef(env, jurl);
        free(h); jenv_release(&L); return NULL;
    }

    h->conn = (*env)->NewGlobalRef(env, conn);
    h->is   = (*env)->NewGlobalRef(env, is);

    (*env)->DeleteLocalRef(env, is);
    (*env)->DeleteLocalRef(env, conn);
    (*env)->DeleteLocalRef(env, urlObj);
    (*env)->DeleteLocalRef(env, jurl);

    LOGI("basis_jni_https: open ok for %s", url);
    jenv_release(&L);
    return h;
}

static int ensure_scratch(JNIEnv* env, https_ctx* h, int want) {
    if (h->scratch && h->scratch_cap >= want) return 0;
    if (h->scratch) {
        (*env)->DeleteGlobalRef(env, h->scratch);
        h->scratch = NULL;
        h->scratch_cap = 0;
    }
    int cap = want < 16384 ? 16384 : want;
    jbyteArray local = (*env)->NewByteArray(env, cap);
    if (!local) return -1;
    h->scratch = (jbyteArray)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    h->scratch_cap = cap;
    return h->scratch ? 0 : -1;
}

int basis_jni_https_read(void* ctx, uint8_t* buf, int len) {
    https_ctx* h = (https_ctx*)ctx;
    if (!h || h->eof || !buf || len <= 0) return 0;

    jenv_lease L; if (jenv_acquire(&L) != 0) return -1;
    JNIEnv* env = L.env;

    if (ensure_scratch(env, h, len) != 0) { jenv_release(&L); return -1; }

    int want = len < h->scratch_cap ? len : h->scratch_cap;
    jint n = (*env)->CallIntMethod(env, h->is, g_ids.is_read, h->scratch, 0, want);
    if ((*env)->ExceptionCheck(env)) {
        log_and_clear_pending(env, "InputStream.read");
        LOGE("basis_jni_https: read exception after %lld bytes", h->total_bytes);
        h->eof = 1;
        jenv_release(&L);
        return -1;
    }
    if (n < 0) {
        LOGI("basis_jni_https: clean EOF after %lld bytes", h->total_bytes);
        h->eof = 1; jenv_release(&L); return 0;
    }
    if (n == 0) {
        /* Java contract says read(byte[], 0, n>0) should not return 0, but some
         * stacks have bugs. Treat as a transient and ask the caller to retry. */
        jenv_release(&L); return 0;
    }
    (*env)->GetByteArrayRegion(env, h->scratch, 0, n, (jbyte*)buf);
    h->total_bytes += n;
    jenv_release(&L);
    return (int)n;
}

void basis_jni_https_close(void* ctx) {
    https_ctx* h = (https_ctx*)ctx;
    if (!h) return;

    jenv_lease L;
    if (jenv_acquire(&L) == 0) {
        JNIEnv* env = L.env;
        if (h->is) {
            (*env)->CallVoidMethod(env, h->is, g_ids.is_close);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteGlobalRef(env, h->is);
        }
        if (h->conn) {
            if ((*env)->IsInstanceOf(env, h->conn, g_ids.http_conn_cls))
                (*env)->CallVoidMethod(env, h->conn, g_ids.http_disconnect);
            if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
            (*env)->DeleteGlobalRef(env, h->conn);
        }
        if (h->scratch) (*env)->DeleteGlobalRef(env, h->scratch);
        jenv_release(&L);
    }
    free(h);
}
