/*
 * basis_win_decode.cpp — Windows OS-codec backend (implements basis_decoder_*).
 *
 * Pipeline:
 *   submit_video (demux thread): feed Annex-B AUs to the Media Foundation H.264/
 *     H.265 decoder MFT running on a DXVA-enabled D3D11 device. Decoded NV12 is
 *     converted to BGRA by an ID3D11VideoProcessor into a keyed-mutex *shared*
 *     texture on the decode device.
 *   render_update (render thread): copy the shared BGRA into the Unity-visible
 *     output texture (created on Unity's device) under the keyed mutex.
 *   submit_audio: feed raw AAC to the AAC decoder MFT -> float PCM -> ring.
 *
 * Graphics targets:
 *   D3D11 — output texture created on Unity's ID3D11Device (BGRA). Primary path.
 *   D3D12 — the shared BGRA is opened on Unity's ID3D12Device via OpenSharedHandle
 *           and handed straight to CreateExternalTexture. See the D3D12 notes by
 *           the present code: cross-API sync uses the keyed mutex where possible;
 *           validate fence/tearing behaviour on real hardware.
 *
 * Notes / iterate-here:
 *   - Uses a synchronous (DXVA) decoder MFT via MFTEnumEx. Async hardware MFTs
 *     (event-driven) would lower latency further but need METransform* handling.
 *   - HEVC requires an installed HEVC decoder MFT (HEVC Video Extensions).
 */

#include "../basis_media_internal.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)
#endif

/* ---- PCM ring ----------------------------------------------------------- */

/* Interleaved float FIFO with per-chunk PTS metadata. The producer (decode
 * thread) writes decoded chunks tagged with their media timestamps; the
 * consumer (Unity audio thread) reads gated against the presentation clock so
 * audio release is paced to the same timeline video presents on. All drops are
 * whole-frame multiples — a non-frame-sized drop would permanently rotate the
 * channel order for every consumer downstream. */
struct PcmRing {
    float* buf = nullptr;
    int cap = 0;     /* in floats */
    int head = 0, tail = 0;
    int frame = 1;   /* floats per interleaved frame (channel count) */
    int sr = 48000;  /* sample rate, for chunk durations */
    CRITICAL_SECTION cs;

    static const int CHUNKS = 1024;
    struct Chunk { int64_t pts; int floats; };
    Chunk chunks[CHUNKS] = {};
    int chead = 0, ccount = 0;

    /* Serving is sequential and never blocks on future timestamps — the
     * consumer's own prefetch owns short-term timing. The clock is only used
     * to detect a stale head (connect burst, post-stall backlog): when the
     * oldest queued audio falls further than TRIM_LATE behind the clock, the
     * queue is trimmed so the head sits CONSUMER_LEAD ahead of it. The lead
     * compensates the consumer-side pipeline (Unity streaming-clip prefetch +
     * DSP output latency): samples handed over now become audible roughly that
     * much later, so a future-biased head lands on the clock at the speaker. */
    static const int64_t TRIM_LATE_US = 150000;
    static const int64_t CONSUMER_LEAD_US = 380000;

    void init(int floats) { cap = floats; buf = (float*)malloc(sizeof(float) * cap); InitializeCriticalSection(&cs); }
    void destroy() { free(buf); buf = nullptr; DeleteCriticalSection(&cs); }

    int fill() const { return (tail - head + cap) % cap; }

    /* Drops the oldest `n` floats (rounded down to whole frames) from the float
     * ring and the chunk metadata together. Caller holds cs. */
    void drop_oldest(int n) {
        n -= n % frame;
        int avail = fill();
        if (n > avail) n = avail - (avail % frame);
        if (n <= 0) return;
        head = (head + n) % cap;
        while (n > 0 && ccount > 0) {
            Chunk& c = chunks[chead];
            if (c.floats <= n) { n -= c.floats; chead = (chead + 1) % CHUNKS; ccount--; }
            else {
                c.floats -= n;
                c.pts += (int64_t)(n / frame) * 1000000LL / (sr > 0 ? sr : 48000);
                n = 0;
            }
        }
    }

    void write(const float* s, int n, int64_t pts) {
        if (n <= 0) return;
        EnterCriticalSection(&cs);
        if (n > cap - 1) {
            /* Drop the oldest whole frames of an over-capacity write and carry
             * the timestamp forward, so the retained tail keeps a correct PTS
             * and the channel order isn't rotated by a sub-frame trim. */
            int keep = (cap - 1) - ((cap - 1) % frame);
            int drop = n - keep;
            s += drop;
            pts += (int64_t)(drop / frame) * 1000000LL / (sr > 0 ? sr : 48000);
            n = keep;
        }
        int space = cap - 1 - fill();
        if (n > space) {
            int need = (n - space) + frame - 1;
            drop_oldest(need - need % frame);
        }
        for (int i = 0; i < n; ++i) { buf[tail] = s[i]; tail = (tail + 1) % cap; }
        if (ccount == CHUNKS) {
            chunks[(chead + ccount - 1) % CHUNKS].floats += n;
        } else {
            Chunk& c = chunks[(chead + ccount) % CHUNKS];
            c.pts = pts; c.floats = n; ccount++;
        }
        LeaveCriticalSection(&cs);
    }

    /* now_us = INT64_MIN reads ungated (no presentation clock yet). */
    int read(float* out, int n, int64_t now_us) {
        EnterCriticalSection(&cs);
        int64_t srr = sr > 0 ? sr : 48000;
        if (now_us != INT64_MIN && ccount > 0) {
            int64_t late = now_us - chunks[chead].pts;
            if (late > TRIM_LATE_US) {
                drop_oldest((int)((late + CONSUMER_LEAD_US) * srr / 1000000LL) * frame);
            }
        }
        int got = 0;
        while (got < n && ccount > 0) {
            Chunk& c = chunks[chead];
            int take = c.floats < n - got ? c.floats : n - got;
            for (int i = 0; i < take; ++i) { out[got + i] = buf[head]; head = (head + 1) % cap; }
            got += take;
            if (take == c.floats) { chead = (chead + 1) % CHUNKS; ccount--; }
            else {
                c.floats -= take;
                c.pts += (int64_t)take * 1000000LL / (frame * srr);
            }
        }
        LeaveCriticalSection(&cs);
        return got;
    }
};

/* ---- decoder ------------------------------------------------------------ */

struct basis_decoder {
    basis_media_engine_t* engine = nullptr;

    /* decode device (DXVA) */
    ID3D11Device* devDec = nullptr;
    ID3D11DeviceContext* ctxDec = nullptr;
    UINT resetToken = 0;
    IMFDXGIDeviceManager* devMgr = nullptr;

    /* video */
    IMFTransform* vdec = nullptr;
    basis_codec_t vcodec = BASIS_CODEC_NONE;
    int vwidth = 0, vheight = 0;
    bool vconfigured = false;

    ID3D11VideoDevice* vdevice = nullptr;
    ID3D11VideoContext* vcontext = nullptr;
    ID3D11VideoProcessor* vproc = nullptr;
    ID3D11VideoProcessorEnumerator* vprocEnum = nullptr;

    /* Ring of BGRA shared buffers (decode device). The decode producer writes
     * round-robin; the render consumer presents frames on a PTS clock so bursty
     * decode delivery is smoothed into steady, framerate-accurate output. Each
     * buffer is a keyed-mutex shared resource (key 0 = free). */
    /* Frame ring. Sized so a normal jitter buffer fits in frames even at very high
     * source rates (32 frames = 533ms @60fps, 128ms @250fps). Present picks the
     * freshest due slot, so the ring only needs to span buffer + decode headroom. */
    static const int RING = 32;
    ID3D11Texture2D* ringTex[RING] = {};
    IDXGIKeyedMutex* ringMutexDec[RING] = {};
    ID3D11VideoProcessorOutputView* ringVpOut[RING] = {};
    HANDLE ringHandle[RING] = {};
    ID3D11Texture2D* ringOnUnity[RING] = {};
    IDXGIKeyedMutex* ringMutexUnity[RING] = {};
    int64_t ringPts[RING] = {};          /* PTS (us) of the frame in each slot; INT64_MIN = empty */
    int sharedW = 0, sharedH = 0;
    volatile LONGLONG writeSeq = 0;   /* total frames written by the producer */

    /* present clock (render thread) */
    LARGE_INTEGER qpcFreq = {};
    bool clockStarted = false;
    LONGLONG wallStartQpc = 0;
    LONGLONG lastRenderQpc = 0;
    int64_t mediaStartUs = 0;
    int64_t lastPresentedPts = INT64_MIN;

    /* audio-master sync: pace video to audio Unity has actually consumed. */
    int64_t videoBasePts = INT64_MIN;        /* PTS of the first video frame (sync origin) */
    volatile LONGLONG audioSamplesRead = 0;  /* per-channel samples pulled by the audio thread */

    /* jitter buffer (how far behind live we present): selectable + dynamic. */
    volatile LONG bufferUs = 120000;         /* current buffer in microseconds */
    volatile LONG bufferMode = 1;            /* 0 = fixed (use bufferUs), 1 = dynamic (auto-tune) */

    /* Unity output texture (single; consumer copies the due ring buffer into it) */
    basis_gfx_api_t api = BASIS_GFX_NONE;
    ID3D11Device* devUnity = nullptr;       /* D3D11 path */
    ID3D11DeviceContext* ctxUnity = nullptr;
    ID3D11Texture2D* outTexD11 = nullptr;   /* CreateExternalTexture target (D3D11) */
    void* outTexD12 = nullptr;              /* ID3D12Resource* (D3D12 path) */

    /* Vertical origin of the published frame: 0 = bottom-left (upright; Unity
     * samples it with no UV flip), 1 = top-left (consumer must flip V). Set once
     * when the video processor is created — 0 if its stream-mirror was actually
     * applied, 1 if this GPU's VP lacks mirror support so the frame stays
     * un-flipped and the consumer corrects it. Defaults to upright (no surprise
     * flip) before the first frame. */
    volatile LONG frameTopLeft = 0;

    volatile LONG frameCounter = 0;
    int64_t lastPtsUs = -1;
    int64_t prevWritePts = INT64_MIN;        /* last frame PTS written to the ring */
    int64_t frameIntervalUs = 0;             /* EMA of inter-frame PTS delta (source frame period) */
    LARGE_INTEGER createQpc = {};            /* engine open time (for time-to-first-frame) */
    volatile LONG ttffMs = -1;               /* ms from open to first presented frame */
    CRITICAL_SECTION presentLock;

    /* debug counters */
    volatile LONG dbg_in_ok = 0, dbg_in_rej = 0, dbg_out = 0, dbg_blit = 0, dbg_drop = 0;
    volatile LONG dbg_render = 0, dbg_copy = 0;
    volatile LONG dbg_acqfail = 0, dbg_nodue = 0, dbg_lagms = 0;

    /* audio */
    IMFTransform* adec = nullptr;
    basis_codec_t acodec = BASIS_CODEC_NONE;
    int asr = 0, ach = 0, aobj = 2;
    int aBits = 32;                 /* output sample bits: 32=float, 16=PCM int */
    bool aconfigured = false;

    /* LPCM bypass (no decoder): convert/reorder straight into the PCM ring. */
    int aLpcmAssign = 0;            /* Blu-ray channel_assignment */
    int aLpcmBits = 16;
    float* aLpcmBuf = nullptr;      /* reusable convert buffer */
    int aLpcmBufCap = 0;            /* in floats */
    volatile LONG dbg_aout = 0;     /* AAC PCM outputs produced */
    PcmRing pcm;
    int64_t aPtsFallback = 0;       /* next chunk PTS when MF gives no sample time */

    /* Audio-gate clock: media-time offset from QPC, low-passed (~2s) so the
     * segment-cadence wobble of the live-edge lock (bursty transports advance
     * `newest` in jumps) averages out before the audio anchor reads it. The
     * audio thread reconstructs `now` as qpc_us + offset. INT64_MIN = clock
     * not started (audio reads ungated). */
    volatile LONGLONG audClockOffsetUs = INT64_MIN;
};

/* ---- D3D / MF helpers --------------------------------------------------- */

static bool create_decode_device(basis_decoder* d) {
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   fl, 2, D3D11_SDK_VERSION, &d->devDec, nullptr, &d->ctxDec);
    if (FAILED(hr)) return false;

    ID3D11Multithread* mt = nullptr;
    if (SUCCEEDED(d->devDec->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }

    hr = MFCreateDXGIDeviceManager(&d->resetToken, &d->devMgr);
    if (FAILED(hr)) return false;
    hr = d->devMgr->ResetDevice(d->devDec, d->resetToken);
    if (FAILED(hr)) return false;

    d->devDec->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&d->vdevice);
    d->ctxDec->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&d->vcontext);
    return d->vdevice && d->vcontext;
}

static const GUID* video_subtype(basis_codec_t c) {
    return (c == BASIS_CODEC_H265) ? &MFVideoFormat_HEVC : &MFVideoFormat_H264;
}

/* Finds a synchronous (DXVA-capable) decoder MFT for the codec. */
static IMFTransform* create_video_mft(basis_codec_t codec) {
    MFT_REGISTER_TYPE_INFO inType = { MFMediaType_Video, *video_subtype(codec) };
    IMFActivate** acts = nullptr;
    UINT32 count = 0;
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inType, nullptr, &acts, &count)) || count == 0)
        return nullptr;
    IMFTransform* mft = nullptr;
    for (UINT32 i = 0; i < count; ++i) {
        if (!mft && SUCCEEDED(acts[i]->ActivateObject(IID_PPV_ARGS(&mft)))) { /* keep first */ }
        acts[i]->Release();
    }
    CoTaskMemFree(acts);
    return mft;
}

static bool configure_video_mft(basis_decoder* d) {
    d->vdec = create_video_mft(d->vcodec);
    if (!d->vdec) { basis_engine_set_error(d->engine, "no Media Foundation decoder MFT for this codec (HEVC needs the HEVC Video Extension)"); return false; }

    /* bind DXVA device manager */
    d->vdec->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)d->devMgr);

    /* input type */
    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, *video_subtype(d->vcodec));
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (d->vwidth > 0 && d->vheight > 0)
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, d->vwidth, d->vheight);
    HRESULT hr = d->vdec->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) { basis_engine_set_error(d->engine, "MFT SetInputType failed"); return false; }

    /* pick an NV12 output type */
    IMFMediaType* out = nullptr;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(d->vdec->GetOutputAvailableType(0, i, &t))) break;
        GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) { out = t; break; }
        t->Release();
    }
    if (!out) {
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    hr = d->vdec->SetOutputType(0, out, 0);
    out->Release();
    if (FAILED(hr)) { basis_engine_set_error(d->engine, "MFT SetOutputType(NV12) failed"); return false; }

    d->vdec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->vdec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

static void release_shared_locked(basis_decoder* d) {
    for (int i = 0; i < basis_decoder::RING; ++i) {
        SAFE_RELEASE(d->ringVpOut[i]);
        SAFE_RELEASE(d->ringMutexDec[i]);
        SAFE_RELEASE(d->ringTex[i]);
        SAFE_RELEASE(d->ringMutexUnity[i]);
        SAFE_RELEASE(d->ringOnUnity[i]);
        if (d->ringHandle[i]) { CloseHandle(d->ringHandle[i]); d->ringHandle[i] = nullptr; }
        d->ringPts[i] = INT64_MIN;
    }
    SAFE_RELEASE(d->outTexD11);
    if (d->outTexD12) { ((ID3D12Resource*)d->outTexD12)->Release(); d->outTexD12 = nullptr; }
    d->writeSeq = 0;
    d->clockStarted = false;
    d->lastPresentedPts = INT64_MIN;
}

/* Allocate the ring of keyed-mutex BGRA buffers on the decode device, open each
 * on Unity's device, and create the single Unity-visible output texture. */
static bool ensure_shared_textures(basis_decoder* d, int w, int h) {
    if (d->ringTex[0] && d->sharedW == w && d->sharedH == h) return true;

    EnterCriticalSection(&d->presentLock);
    release_shared_locked(d);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    bool ok = true;
    for (int i = 0; i < basis_decoder::RING && ok; ++i) {
        if (FAILED(d->devDec->CreateTexture2D(&desc, nullptr, &d->ringTex[i]))) { ok = false; break; }
        d->ringTex[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&d->ringMutexDec[i]);

        IDXGIResource1* res1 = nullptr;
        if (SUCCEEDED(d->ringTex[i]->QueryInterface(__uuidof(IDXGIResource1), (void**)&res1))) {
            res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &d->ringHandle[i]);
            res1->Release();
        }
        if (d->api == BASIS_GFX_D3D11 && d->devUnity && d->ringHandle[i]) {
            ID3D11Device1* dev1 = nullptr;
            if (SUCCEEDED(d->devUnity->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) {
                dev1->OpenSharedResource1(d->ringHandle[i], __uuidof(ID3D11Texture2D), (void**)&d->ringOnUnity[i]);
                dev1->Release();
            }
            if (d->ringOnUnity[i])
                d->ringOnUnity[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&d->ringMutexUnity[i]);
        }
        d->ringPts[i] = INT64_MIN;
    }

    d->sharedW = w; d->sharedH = h;

    /* Unity-visible output texture (TYPELESS so Unity makes a UNORM or sRGB SRV as
     * its colour space needs; a typed UNORM fails sRGB SRV creation with 0x80070057). */
    if (ok && d->api == BASIS_GFX_D3D11 && d->devUnity) {
        D3D11_TEXTURE2D_DESC od = desc;
        od.MiscFlags = 0;
        od.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        od.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        if (FAILED(d->devUnity->CreateTexture2D(&od, nullptr, &d->outTexD11))) {
            basis_engine_set_error(d->engine, "failed to create Unity output texture");
            ok = false;
        }
    }
    /* D3D12: outTexD12 opened lazily in render_update from a buffer's handle. */

    LeaveCriticalSection(&d->presentLock);
    return ok;
}

/* NV12 (decode device) -> next ring BGRA buffer via the video processor. */
static void video_process_to_shared(basis_decoder* d, ID3D11Texture2D* nv12, UINT arrayIndex, int64_t pts_us) {
    D3D11_TEXTURE2D_DESC td; nv12->GetDesc(&td);
    int w = (int)td.Width, h = (int)td.Height;
    if (d->vwidth != w || d->vheight != h) { d->vwidth = w; d->vheight = h; }
    if (!ensure_shared_textures(d, w, h)) return;
    if (d->videoBasePts == INT64_MIN) d->videoBasePts = pts_us; /* sync origin */

    if (!d->vproc) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = w; cd.InputHeight = h; cd.OutputWidth = w; cd.OutputHeight = h;
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(d->vdevice->CreateVideoProcessorEnumerator(&cd, &d->vprocEnum))) return;
        if (FAILED(d->vdevice->CreateVideoProcessor(d->vprocEnum, 0, &d->vproc))) return;
        /* Try to make the video processor emit a bottom-left origin frame so Unity
         * samples it right-way-up with no UV flip. VideoProcessorSetStreamMirror is
         * an OPTIONAL feature: a GPU's VP advertises it via the MIRROR caps bit, and
         * drivers that lack it (some Intel iGPU / WARP / virtualized adapters)
         * silently ignore the call — that is the "video is upside-down only on some
         * machines" bug, since the method returns void so the no-op is invisible. So
         * gate on the cap: when the mirror actually runs, mark the frame upright;
         * otherwise leave it top-left and report that origin so the consumer applies
         * a free, deterministic UV flip instead. VideoProcessorSetStreamMirror lives
         * on ID3D11VideoContext1 (D3D11.1+), so query it from the base context. */
        bool mirrored = false;
        D3D11_VIDEO_PROCESSOR_CAPS vpcaps = {};
        bool canMirror = SUCCEEDED(d->vprocEnum->GetVideoProcessorCaps(&vpcaps)) &&
                         (vpcaps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_MIRROR);
        if (canMirror) {
            ID3D11VideoContext1* vctx1 = nullptr;
            if (SUCCEEDED(d->vcontext->QueryInterface(__uuidof(ID3D11VideoContext1), (void**)&vctx1)) && vctx1) {
                vctx1->VideoProcessorSetStreamMirror(d->vproc, 0, TRUE, FALSE, TRUE);
                vctx1->Release();
                mirrored = true;
            }
        }
        d->frameTopLeft = mirrored ? 0 : 1;
    }

    int slot = (int)(d->writeSeq % basis_decoder::RING);

    if (!d->ringVpOut[slot]) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(d->vdevice->CreateVideoProcessorOutputView(d->ringTex[slot], d->vprocEnum, &ovd, &d->ringVpOut[slot]))) return;
    }

    ID3D11VideoProcessorInputView* inView = nullptr;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = arrayIndex;
    if (FAILED(d->vdevice->CreateVideoProcessorInputView(nv12, d->vprocEnum, &ivd, &inView))) return;

    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = inView;

    /* key 0 = free. Short wait then drop — only contends if the consumer is still
     * reading THIS slot (i.e. it lagged a whole ring), which the PTS pacing avoids.
     * Never stalls the demux/network thread. */
    if (d->ringMutexDec[slot]) {
        if (d->ringMutexDec[slot]->AcquireSync(0, 4) != S_OK) { InterlockedIncrement(&d->dbg_drop); inView->Release(); return; }
    }
    d->vcontext->VideoProcessorBlt(d->vproc, d->ringVpOut[slot], 0, 1, &stream);
    d->ctxDec->Flush();
    if (d->ringMutexDec[slot]) d->ringMutexDec[slot]->ReleaseSync(0);

    /* track the source frame period (EMA), ignoring discontinuities, so the pacer
     * can size the buffer in time while keeping it within the ring's frame span. */
    if (d->prevWritePts != INT64_MIN) {
        int64_t dlt = pts_us - d->prevWritePts;
        if (dlt > 0 && dlt < 1000000)
            d->frameIntervalUs = d->frameIntervalUs ? (d->frameIntervalUs * 7 + dlt) / 8 : dlt;
    }
    d->prevWritePts = pts_us;

    /* publish: stamp PTS (aligned int64 write is atomic on x64), then bump seq. */
    d->ringPts[slot] = pts_us;
    InterlockedIncrement64(&d->writeSeq);
    InterlockedIncrement(&d->frameCounter);
    InterlockedIncrement(&d->dbg_blit);
    inView->Release();
}

/* Pull all currently-available output samples from the video MFT.
 * CRITICAL: in DXVA mode the MFT hands us its own IMFSample in outBuf.pSample,
 * backed by a small pool of D3D11 surfaces. That sample MUST be released every
 * iteration or the pool drains and ProcessOutput returns NEED_MORE_INPUT forever
 * (the "one frame then stall" bug). We release outBuf.pSample on every path. */
static void drain_video(basis_decoder* d) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO si = {};
        d->vdec->GetOutputStreamInfo(0, &si);
        bool providesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.dwStreamID = 0;
        if (!providesSamples) {
            IMFSample* s = nullptr; IMFMediaBuffer* mb = nullptr;
            MFCreateSample(&s);
            MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (DWORD)(d->vwidth * d->vheight * 3), &mb);
            s->AddBuffer(mb); mb->Release();
            outBuf.pSample = s;
        }

        DWORD status = 0;
        HRESULT hr = d->vdec->ProcessOutput(0, 1, &outBuf, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            SAFE_RELEASE(outBuf.pSample);
            if (outBuf.pEvents) outBuf.pEvents->Release();
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            IMFMediaType* t = nullptr;
            for (DWORD i = 0; ; ++i) {
                IMFMediaType* c = nullptr;
                if (FAILED(d->vdec->GetOutputAvailableType(0, i, &c))) break;
                GUID sub; c->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) { t = c; break; }
                c->Release();
            }
            if (t) { d->vdec->SetOutputType(0, t, 0); t->Release(); }
            SAFE_RELEASE(outBuf.pSample);
            if (outBuf.pEvents) outBuf.pEvents->Release();
            continue;
        }
        if (FAILED(hr)) {
            SAFE_RELEASE(outBuf.pSample);
            if (outBuf.pEvents) outBuf.pEvents->Release();
            break;
        }

        IMFSample* outSample = outBuf.pSample;
        if (outSample) {
            InterlockedIncrement(&d->dbg_out);
            LONGLONG ts = 0;
            if (SUCCEEDED(outSample->GetSampleTime(&ts))) d->lastPtsUs = ts / 10; /* 100ns -> us */

            IMFMediaBuffer* mb = nullptr;
            if (SUCCEEDED(outSample->GetBufferByIndex(0, &mb))) {
                IMFDXGIBuffer* dxgi = nullptr;
                if (SUCCEEDED(mb->QueryInterface(__uuidof(IMFDXGIBuffer), (void**)&dxgi))) {
                    ID3D11Texture2D* tex = nullptr;
                    UINT subIndex = 0;
                    dxgi->GetResource(__uuidof(ID3D11Texture2D), (void**)&tex);
                    dxgi->GetSubresourceIndex(&subIndex);
                    if (tex) { video_process_to_shared(d, tex, subIndex, d->lastPtsUs); tex->Release(); }
                    dxgi->Release();
                }
                mb->Release();
            }
        }

        SAFE_RELEASE(outBuf.pSample);   /* releases MFT-provided OR locally-allocated sample */
        if (outBuf.pEvents) outBuf.pEvents->Release();
    }
}

/* ---- audio MFT (AAC -> float PCM) -------------------------------------- */

/* Configures the in-box AAC decoder MFT. Fails silently (audio stays muted, video
 * unaffected) — never errors the engine. aconfigured/aout in the debug string say
 * whether it worked. */
static bool configure_audio_mft(basis_decoder* d, const uint8_t* asc, int asc_len) {
    if (FAILED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d->adec))))
        return false;

    IMFMediaType* in = nullptr;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, d->asr ? d->asr : 48000);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, d->ach ? d->ach : 2);
    in->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); /* raw AAC frames */
    {
        /* MF_MT_USER_DATA = HEAACWAVEINFO bytes after WAVEFORMATEX (12) + ASC. */
        uint8_t blob[64] = {0};
        int n = 12;
        if (asc && asc_len > 0 && 12 + asc_len <= (int)sizeof(blob)) { memcpy(blob + 12, asc, asc_len); n = 12 + asc_len; }
        in->SetBlob(MF_MT_USER_DATA, blob, n);
    }
    HRESULT hr = d->adec->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) { SAFE_RELEASE(d->adec); return false; }

    /* Pick the output type the decoder offers. Prefer a channel count matching
     * the input, then IEEE float, then more channels. For >2-channel AAC the
     * decoder also offers a stereo fold-down, so matching the input channel
     * count is what keeps the discrete surround channels (e.g. 5.1); float vs
     * 16-bit PCM only changes the conversion in drain_audio. */
    IMFMediaType* chosen = nullptr; int bits = 0; int chosenRank = -1;
    int target = d->ach ? d->ach : 2;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(d->adec->GetOutputAvailableType(0, i, &t))) break;
        GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 b = 0, tch = 0;
        t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &b);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &tch);
        bool isFloat = (sub == MFAudioFormat_Float);
        bool isPcm = (sub == MFAudioFormat_PCM);
        if (!isFloat && !isPcm) { t->Release(); continue; }
        int rank = ((int)tch == target ? 10000 : 0) + (isFloat ? 1000 : 0) + (int)tch;
        if (rank > chosenRank) {
            if (chosen) chosen->Release();
            chosen = t; chosenRank = rank;
            bits = isFloat ? 32 : (int)(b ? b : 16);
        } else {
            t->Release();
        }
    }
    if (!chosen) { SAFE_RELEASE(d->adec); return false; }

    UINT32 sr = 0, ch = 0;
    chosen->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    chosen->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    if (sr) d->asr = (int)sr;
    if (ch) d->ach = (int)ch;
    d->aBits = (bits == 16) ? 16 : 32;

    hr = d->adec->SetOutputType(0, chosen, 0);
    chosen->Release();
    if (FAILED(hr)) { SAFE_RELEASE(d->adec); return false; }

    d->adec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->adec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

static void drain_audio(basis_decoder* d) {
    for (;;) {
        MFT_OUTPUT_STREAM_INFO si = {};
        d->adec->GetOutputStreamInfo(0, &si);
        IMFSample* sample = nullptr; IMFMediaBuffer* mb = nullptr;
        MFCreateSample(&sample);
        MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 65536, &mb);
        sample->AddBuffer(mb);

        MFT_OUTPUT_DATA_BUFFER ob = {}; ob.pSample = sample; DWORD status = 0;
        HRESULT hr = d->adec->ProcessOutput(0, 1, &ob, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) { mb->Release(); sample->Release(); break; }

        /* The decoder propagates input sample times to its outputs; fall back
         * to a sample-counted timeline if one comes back without a time. */
        LONGLONG st100 = 0;
        int64_t pts = SUCCEEDED(sample->GetSampleTime(&st100)) ? st100 / 10 : d->aPtsFallback;

        BYTE* p = nullptr; DWORD cur = 0;
        if (SUCCEEDED(mb->Lock(&p, nullptr, &cur)) && cur > 0) {
            int srr = d->asr > 0 ? d->asr : 48000;
            int ch = d->ach > 0 ? d->ach : 1;
            if (d->aBits == 16) {
                int n = (int)(cur / 2);
                const int16_t* s16 = (const int16_t*)p;
                float tmp[4096];
                int maxFrames = 4096 / ch; if (maxFrames < 1) maxFrames = 1;
                int off = 0;
                /* Write whole interleaved frames only: a sub-frame chunk would
                 * give the ring's per-chunk PTS a fractional sample count. */
                while (off + ch <= n) {
                    int framesLeft = (n - off) / ch;
                    int c = (framesLeft > maxFrames ? maxFrames : framesLeft) * ch;
                    for (int i = 0; i < c; ++i) tmp[i] = s16[off + i] / 32768.0f;
                    d->pcm.write(tmp, c, pts + (int64_t)(off / ch) * 1000000LL / srr);
                    off += c;
                }
                d->aPtsFallback = pts + (int64_t)(n / ch) * 1000000LL / srr;
            } else {
                int n = (int)(cur / sizeof(float));
                d->pcm.write((const float*)p, n, pts);
                d->aPtsFallback = pts + (int64_t)(n / ch) * 1000000LL / srr;
            }
            mb->Unlock();
            InterlockedIncrement(&d->dbg_aout);
        }
        mb->Release(); sample->Release();
        if (ob.pEvents) ob.pEvents->Release();
    }
}

/* ---- internal API impl -------------------------------------------------- */

extern "C" basis_decoder_t* basis_decoder_create(basis_media_engine_t* engine) {
    static bool mfStarted = false;
    if (!mfStarted) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); MFStartup(MF_VERSION); mfStarted = true; }

    basis_decoder* d = new basis_decoder();
    d->engine = engine;
    d->api = basis_gfx_get_api();
    d->devUnity = (ID3D11Device*)basis_gfx_get_d3d11_device();
    if (d->devUnity) d->devUnity->GetImmediateContext(&d->ctxUnity);
    InitializeCriticalSection(&d->presentLock);
    QueryPerformanceFrequency(&d->qpcFreq);
    QueryPerformanceCounter(&d->createQpc);
    for (int i = 0; i < basis_decoder::RING; ++i) d->ringPts[i] = INT64_MIN;
    d->pcm.init(48000 * 2 * 4); /* ~4s stereo */

    if (!create_decode_device(d)) {
        basis_engine_set_error(engine, "failed to create DXVA D3D11 decode device");
        /* keep the object; audio/video setup will fail gracefully */
    }
    return d;
}

extern "C" void basis_decoder_destroy(basis_decoder_t* d) {
    if (!d) return;
    basis_decoder_render_release(d); /* idempotent GPU teardown */

    SAFE_RELEASE(d->vproc);
    SAFE_RELEASE(d->vprocEnum);
    SAFE_RELEASE(d->vcontext);
    SAFE_RELEASE(d->vdevice);
    SAFE_RELEASE(d->vdec);
    SAFE_RELEASE(d->adec);
    SAFE_RELEASE(d->devMgr);
    SAFE_RELEASE(d->ctxDec);
    SAFE_RELEASE(d->devDec);
    SAFE_RELEASE(d->ctxUnity);
    /* shared textures + handles already freed by basis_decoder_render_release above */
    DeleteCriticalSection(&d->presentLock);
    d->pcm.destroy();
    free(d->aLpcmBuf);
    delete d;
}

extern "C" int basis_decoder_set_video_format(basis_decoder_t* d, basis_codec_t codec,
                                              const uint8_t* extradata, int extradata_len, int w, int h) {
    if (!d || d->vconfigured) return 0;
    d->vcodec = codec; d->vwidth = w; d->vheight = h;
    if (!d->devDec) return -1;
    if (!configure_video_mft(d)) return -1;
    /* Feed SPS/PPS (Annex B extradata) as the first input so the MFT has config. */
    if (extradata && extradata_len > 0) basis_decoder_submit_video(d, extradata, extradata_len, 0, 0);
    d->vconfigured = true;
    return 0;
}

extern "C" int basis_decoder_set_audio_format(basis_decoder_t* d, basis_codec_t codec,
                                              int sample_rate, int channels, const uint8_t* asc, int asc_len) {
    if (!d || d->aconfigured) return 0;

    if (codec == BASIS_CODEC_LPCM) {
        /* No decoder involved — submit_audio converts straight into the ring.
         * 48 kHz / 16- or 24-bit only (the streaming-clip consumer plays at the
         * clip rate, so 96/192 kHz needs a resampler this player doesn't have
         * yet). The TS demuxer already filters to these formats before
         * announcing; this guard is the matching backstop. The config blob
         * carries the Blu-ray channel_assignment + bits code. */
        if (sample_rate != 48000 || channels < 1 || channels > 8 || asc_len < 2) return 0;
        int bits = asc[1] == 1 ? 16 : asc[1] == 3 ? 24 : 0;
        if (!bits) return 0; /* 20-bit unsupported */
        d->acodec = BASIS_CODEC_LPCM;
        d->asr = sample_rate; d->ach = channels;
        d->aLpcmAssign = asc[0];
        d->aLpcmBits = bits;
        d->aconfigured = true;
        d->pcm.frame = channels;
        d->pcm.sr = sample_rate;
        return 0;
    }

    if (codec != BASIS_CODEC_AAC) return 0;
    d->asr = sample_rate; d->ach = channels;
    if (configure_audio_mft(d, asc, asc_len)) {
        d->acodec = BASIS_CODEC_AAC;
        d->aconfigured = true;
        d->pcm.frame = d->ach > 0 ? d->ach : 1;
        d->pcm.sr = d->asr > 0 ? d->asr : 48000;
    }
    return 0;
}

static IMFSample* make_input_sample(const uint8_t* data, int len, int64_t pts_us) {
    IMFSample* s = nullptr; IMFMediaBuffer* b = nullptr;
    MFCreateSample(&s);
    MFCreateMemoryBuffer(len, &b);
    BYTE* p = nullptr; DWORD maxlen = 0;
    b->Lock(&p, &maxlen, nullptr);
    memcpy(p, data, len);
    b->Unlock();
    b->SetCurrentLength(len);
    s->AddBuffer(b);
    s->SetSampleTime((LONGLONG)pts_us * 10); /* us -> 100ns */
    b->Release();
    return s;
}

/* Split-stream thread-safety: submit_video (video demux thread) and submit_audio (audio
 * demux thread) can run concurrently. They are safe by separation — distinct MFTs (vdec vs
 * adec) feeding distinct outputs (the video frame path vs the PCM ring), with no shared
 * mutable state between them and atomic (Interlocked) debug counters. The render thread reads
 * each output under its own lock. Keep video-path and audio-path state disjoint to preserve
 * this; if that ever changes, serialise submission through a decoder mutex. */
extern "C" int basis_decoder_submit_video(basis_decoder_t* d, const uint8_t* annexb, int len, int64_t pts_us, int key) {
    (void)key;
    if (!d || !d->vdec || !annexb || len <= 0) return -1;
    IMFSample* s = make_input_sample(annexb, len, pts_us);

    /* Feed the AU, draining output to make room rather than dropping it. The
     * decoder must accept every frame or playback decimates to the rate at which
     * the input queue happens to have room. */
    bool consumed = false;
    for (int attempt = 0; attempt < 16 && !consumed; ++attempt) {
        HRESULT hr = d->vdec->ProcessInput(0, s, 0);
        if (hr == MF_E_NOTACCEPTING) {
            InterlockedIncrement(&d->dbg_in_rej);
            drain_video(d); /* pull outputs to free input slots, then retry */
        } else {
            if (SUCCEEDED(hr)) InterlockedIncrement(&d->dbg_in_ok);
            consumed = true;
        }
    }
    s->Release();
    drain_video(d);
    return 0;
}

/* Source-order -> WAVE-order channel map for the Blu-ray HDMV LPCM
 * channel_assignment values whose stream order differs from WAVE (Blu-ray
 * places the LFE last and the side pair before the rears). The index tables
 * match ffmpeg's pcm_bluray decoder remap for assignments 9 (5.1), 10 (7.0)
 * and 11 (7.1), and were verified by ear against a 7.1 channel-marker stream.
 * NULL = identity (mono/stereo/3.0/4.0/5.0 already arrive in WAVE order). */
static const int* lpcm_remap(int assign) {
    static const int k51[6] = { 0, 1, 2, 4, 5, 3 };
    static const int k70[7] = { 0, 1, 2, 5, 3, 4, 6 };
    static const int k71[8] = { 0, 1, 2, 6, 4, 5, 7, 3 };
    if (assign == 9) return k51;
    if (assign == 10) return k70;
    if (assign == 11) return k71;
    return nullptr;
}

static void submit_lpcm(basis_decoder* d, const uint8_t* p, int len, int64_t pts_us) {
    int ch = d->ach;
    int bytes = d->aLpcmBits / 8;
    int frame_bytes = ch * bytes;
    int frames = len / frame_bytes;
    if (frames <= 0) return;
    int floats = frames * ch;
    if (floats > d->aLpcmBufCap) {
        float* nb = (float*)realloc(d->aLpcmBuf, sizeof(float) * floats);
        if (!nb) return;
        d->aLpcmBuf = nb; d->aLpcmBufCap = floats;
    }
    const int* map = lpcm_remap(d->aLpcmAssign);
    for (int f = 0; f < frames; ++f) {
        const uint8_t* s = p + f * frame_bytes;
        float* o = d->aLpcmBuf + f * ch;
        for (int c = 0; c < ch; ++c) {
            int oc = map ? map[c] : c;
            if (bytes == 2) {
                int v = (int16_t)((s[c * 2] << 8) | s[c * 2 + 1]);
                o[oc] = v / 32768.0f;
            } else {
                int v = (s[c * 3] << 16) | (s[c * 3 + 1] << 8) | s[c * 3 + 2];
                if (v & 0x800000) v -= 0x1000000;
                o[oc] = v / 8388608.0f;
            }
        }
    }
    d->pcm.write(d->aLpcmBuf, floats, pts_us);
    InterlockedIncrement(&d->dbg_aout);
}

extern "C" int basis_decoder_submit_audio(basis_decoder_t* d, const uint8_t* data, int len, int64_t pts_us) {
    if (!d || !data || len <= 0) return -1;
    if (d->acodec == BASIS_CODEC_LPCM) { submit_lpcm(d, data, len, pts_us); return 0; }
    if (!d->adec) return -1;
    IMFSample* s = make_input_sample(data, len, pts_us);
    HRESULT hr = d->adec->ProcessInput(0, s, 0);
    s->Release();
    if (hr == MF_E_NOTACCEPTING) { drain_audio(d); }
    drain_audio(d);
    return 0;
}

extern "C" int basis_decoder_try_open_url(basis_decoder_t* d, const char* url) {
    (void)d; (void)url;
    return 0; /* Windows always uses the core demuxers + WinHTTP */
}

/* ---- render thread ------------------------------------------------------ */

extern "C" int basis_decoder_render_update(basis_decoder_t* d) {
    if (!d) return -1;
    InterlockedIncrement(&d->dbg_render);
    if (basis_engine_is_paused(d->engine)) return 0;
    if (d->writeSeq == 0) return 0;

    LARGE_INTEGER nowq; QueryPerformanceCounter(&nowq);
    EnterCriticalSection(&d->presentLock);

    /* newest available PTS in the ring */
    int64_t newest = INT64_MIN;
    for (int i = 0; i < basis_decoder::RING; ++i) if (d->ringPts[i] > newest) newest = d->ringPts[i];
    if (newest == INT64_MIN) { LeaveCriticalSection(&d->presentLock); return 0; }

    /* Presentation clock, locked to the live decode edge. The wall clock (QPC)
     * gives smooth, monotonic advance at real rate; a low-pass correction
     * (~0.25s) pulls it toward `newest` (freshest decoded PTS) every render. This
     * fixes the one-shot anchor's drift: that version let the clock run ahead of
     * the frames actually arriving, so almost nothing was ever "due" (nodue spikes,
     * present rate collapsed to ~60% of decode). The correction is fast enough to
     * track bursty live sources without chasing single-frame jitter (the jitter
     * buffer below absorbs those). Large gaps (startup, rebuffer, discontinuity)
     * hard-resync. */
    int64_t freq = d->qpcFreq.QuadPart ? d->qpcFreq.QuadPart : 1;
    bool paced = basis_engine_is_paced(d->engine) != 0;
    int64_t nowMedia;

    if (paced) {
        /* Paced (VOD) clock: a SMOOTH wall clock, gently low-passed toward the decode
         * edge, presenting a fixed buffer behind it. The wall base gives steady,
         * monotonic advance so the present point crosses one frame per frame-interval
         * and every frame is shown (slaving nowMedia directly to `newest` instead makes
         * it jump in the decoder's output bursts, and "present newest due" then skips
         * the frames in between — full 1x position but a low visible framerate). The
         * low-pass also absorbs the startup pipeline-fill offset so the clock settles
         * ~buffer behind the edge rather than a whole ring behind it.
         *
         * This reuses the live clock's wall+low-pass smoothing but tuned for VOD: a
         * fixed small buffer (no 460ms floor / dynamic sizing) and the audio gate
         * published directly (no 2s EMA). Delivery is throttled to ~1x upstream, so the
         * edge never leaps and the hard-resync below only fires on a real discontinuity
         * (loop/seek/long stall), never the per-segment wobble that destabilises live. */
        const int64_t PACED_BUFFER_US = 250000;
        if (!d->clockStarted) {
            d->clockStarted = true;
            d->wallStartQpc = nowq.QuadPart;
            d->lastRenderQpc = nowq.QuadPart;
            d->mediaStartUs = newest;
            d->lastPresentedPts = INT64_MIN;
        }
        int64_t dtUs = (nowq.QuadPart - d->lastRenderQpc) * 1000000LL / freq;
        d->lastRenderQpc = nowq.QuadPart;
        if (dtUs < 0) dtUs = 0; else if (dtUs > 1000000) dtUs = 1000000;
        int64_t clk = d->mediaStartUs + (nowq.QuadPart - d->wallStartQpc) * 1000000LL / freq;
        int64_t err = newest - clk;
        if (err > 1000000 || err < -1000000) {        /* discontinuity / long stall: resync */
            d->wallStartQpc = nowq.QuadPart;
            d->mediaStartUs = newest;
            d->lastPresentedPts = INT64_MIN;
            clk = newest;
        } else {
            int64_t corr = err * dtUs / 250000;        /* ~0.25s lock toward the edge */
            d->mediaStartUs += corr;
            clk += corr;
        }
        nowMedia = clk - PACED_BUFFER_US;
        d->dbg_lagms = (LONG)((newest - nowMedia) / 1000);
        int64_t qpcUs = (nowq.QuadPart - d->createQpc.QuadPart) * 1000000LL / freq;
        InterlockedExchange64(&d->audClockOffsetUs, nowMedia - qpcUs);
    } else {
    if (!d->clockStarted) {
        d->clockStarted = true;
        d->wallStartQpc = nowq.QuadPart;
        d->lastRenderQpc = nowq.QuadPart;
        d->mediaStartUs = newest;
        d->lastPresentedPts = INT64_MIN;
    }
    int64_t dtUs = (nowq.QuadPart - d->lastRenderQpc) * 1000000LL / freq;
    d->lastRenderQpc = nowq.QuadPart;
    if (dtUs < 0) dtUs = 0; else if (dtUs > 1000000) dtUs = 1000000;

    int64_t liveClock = d->mediaStartUs + (int64_t)((nowq.QuadPart - d->wallStartQpc) * 1000000LL / freq);
    int64_t err = newest - liveClock;            /* >0: clock behind the live edge */
    if (err > 700000 || err < -700000) {
        d->wallStartQpc = nowq.QuadPart;
        d->mediaStartUs = newest;
        d->lastPresentedPts = INT64_MIN;
        liveClock = newest;
    } else {
        int64_t corr = err * dtUs / 250000;      /* TAU ~0.25s lock toward live */
        d->mediaStartUs += corr;
        liveClock += corr;
    }

    /* Jitter buffer: present this far behind the live edge. Capped to the ring's
     * frame span so the decoder can't lap the presenter — a fixed-ms buffer would
     * overrun the ring at high source rates, so the ceiling scales with the source
     * frame period (e.g. 120ms is fine at 60fps but clamps near 100ms at 250fps).
     * Dynamic mode grows fast on underrun risk and shrinks symmetrically when
     * over-buffered, with a 200ms hysteresis to avoid grow/shrink fighting. */
    int64_t interval = d->frameIntervalUs > 0 ? d->frameIntervalUs : 16666;
    int64_t maxBuf = (int64_t)(basis_decoder::RING - 6) * interval;
    if (maxBuf < 60000) maxBuf = 60000;
    int64_t buf = d->bufferUs;
    int64_t fill = newest - (liveClock - buf);
    if (d->bufferMode == 1) {
        if (fill < 2 * interval) buf += interval;
        else if (fill > buf + 200000) buf -= 10000;
    }
    /* With audio configured, the buffer must cover the audio consumer's
     * pipeline depth (streaming-clip prefetch + DSP latency, ~400ms): audio
     * cannot be released from ahead of the live decode edge, so video must
     * present at least that far behind it for the two to land together. */
    int64_t minBuf = d->aconfigured ? 460000 : 40000;
    if (buf < minBuf) buf = minBuf;
    if (buf > maxBuf) buf = maxBuf;
    d->bufferUs = (LONG)buf;

    /* Fast start: ramp the effective cushion from ~0 up to the target over the
     * first ~1.2s, so the first decoded frame is presented almost immediately
     * instead of waiting a full buffer behind live, then settle into the full
     * buffer. wallElapsed resets on a hard resync, so a rebuffer re-primes too. */
    int64_t wallElapsed = (int64_t)((nowq.QuadPart - d->wallStartQpc) * 1000000LL / freq);
    int64_t effBuf = (wallElapsed < 1200000) ? (buf * wallElapsed / 1200000) : buf;
    nowMedia = liveClock - effBuf;
    d->dbg_lagms = (LONG)((newest - nowMedia) / 1000);

    /* Publish the audio-gate clock as a low-passed offset from QPC (~2s EMA);
     * large jumps (startup, hard resync, discontinuity) snap instead of
     * filtering so the gate follows resyncs immediately. */
    {
        int64_t qpcUs = (nowq.QuadPart - d->createQpc.QuadPart) * 1000000LL / freq;
        int64_t off = nowMedia - qpcUs;
        LONGLONG prev = InterlockedCompareExchange64(&d->audClockOffsetUs, 0, 0);
        if (prev == INT64_MIN || off - prev > 700000 || off - prev < -700000) {
            InterlockedExchange64(&d->audClockOffsetUs, off);
        } else {
            InterlockedExchange64(&d->audClockOffsetUs, prev + (off - prev) * dtUs / 2000000);
        }
    }
    }

    /* recover from non-monotonic/bogus PTS (lastPresentedPts stuck above the ring) */
    if (d->lastPresentedPts != INT64_MIN && d->lastPresentedPts > newest) d->lastPresentedPts = INT64_MIN;

    /* Present the latest frame that is due (PTS <= now) and newer than the last shown. */
    int best = -1; int64_t bestPts = d->lastPresentedPts;
    for (int i = 0; i < basis_decoder::RING; ++i) {
        int64_t p = d->ringPts[i];
        if (p == INT64_MIN) continue;
        if (p > bestPts && p <= nowMedia) { best = i; bestPts = p; }
    }
    if (best < 0) { InterlockedIncrement(&d->dbg_nodue); LeaveCriticalSection(&d->presentLock); return 0; }

    if (d->api == BASIS_GFX_D3D11 && d->outTexD11 && d->ringOnUnity[best] && d->ctxUnity) {
        HRESULT a = d->ringMutexUnity[best] ? d->ringMutexUnity[best]->AcquireSync(0, 8) : S_OK;
        if (a == S_OK) {
            d->ctxUnity->CopyResource(d->outTexD11, d->ringOnUnity[best]);
            if (d->ringMutexUnity[best]) d->ringMutexUnity[best]->ReleaseSync(0);
            d->lastPresentedPts = bestPts;
            InterlockedIncrement(&d->dbg_copy);
            if (d->ttffMs < 0) {
                LARGE_INTEGER tnow; QueryPerformanceCounter(&tnow);
                d->ttffMs = (LONG)((tnow.QuadPart - d->createQpc.QuadPart) * 1000 / freq);
            }
        } else {
            InterlockedIncrement(&d->dbg_acqfail);
        }
    } else if (d->api == BASIS_GFX_D3D12 && !d->outTexD12 && d->ringHandle[best]) {
        ID3D12Device* dev12 = (ID3D12Device*)basis_gfx_get_d3d12_device();
        if (dev12) {
            ID3D12Resource* res = nullptr;
            if (SUCCEEDED(dev12->OpenSharedHandle(d->ringHandle[best], IID_PPV_ARGS(&res)))) d->outTexD12 = res;
        }
    }
    LeaveCriticalSection(&d->presentLock);
    return 0;
}

extern "C" void basis_decoder_render_release(basis_decoder_t* d) {
    if (!d) return;
    EnterCriticalSection(&d->presentLock);
    release_shared_locked(d);
    d->sharedW = d->sharedH = 0;
    LeaveCriticalSection(&d->presentLock);
}

extern "C" void* basis_decoder_get_texture(basis_decoder_t* d, int* w, int* h) {
    if (!d) return nullptr;
    if (w) *w = d->sharedW;
    if (h) *h = d->sharedH;
    if (d->api == BASIS_GFX_D3D12) return d->outTexD12;
    return d->outTexD11;
}

extern "C" uint64_t basis_decoder_get_frame_counter(basis_decoder_t* d) {
    return d ? (uint64_t)d->frameCounter : 0;
}
extern "C" int basis_decoder_get_video_size(basis_decoder_t* d, int* w, int* h) {
    if (!d || d->sharedW <= 0) return -1;
    if (w) *w = d->sharedW; if (h) *h = d->sharedH; return 0;
}
extern "C" int basis_decoder_get_frame_origin(basis_decoder_t* d) { return d ? (int)d->frameTopLeft : 0; }
extern "C" int64_t basis_decoder_get_position_us(basis_decoder_t* d) { return d ? d->lastPtsUs : -1; }
extern "C" int basis_decoder_get_audio_format(basis_decoder_t* d, int* r, int* c) {
    if (!d || !d->aconfigured) return -1;
    if (r) *r = d->asr ? d->asr : 48000;
    if (c) *c = d->ach ? d->ach : 2;
    return 0;
}
extern "C" int basis_decoder_read_audio(basis_decoder_t* d, float* out, int max_floats) {
    if (!d) return 0;
    if (basis_engine_is_paused(d->engine)) return 0;
    /* Reconstruct the presentation clock from the published offset so audio
     * release is paced to the timeline video presents on. No offset yet (no
     * video frame presented, or an audio-only stream) reads ungated. */
    int64_t now = INT64_MIN;
    LONGLONG off = InterlockedCompareExchange64(&d->audClockOffsetUs, 0, 0);
    if (off != INT64_MIN) {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        int64_t freq = d->qpcFreq.QuadPart ? d->qpcFreq.QuadPart : 1;
        now = (q.QuadPart - d->createQpc.QuadPart) * 1000000LL / freq + off;
    }
    int n = d->pcm.read(out, max_floats, now);
    if (n > 0 && d->ach > 0) InterlockedAdd64(&d->audioSamplesRead, (LONGLONG)(n / d->ach));
    return n;
}

extern "C" void basis_decoder_set_buffer(basis_decoder_t* d, int mode, int buffer_ms) {
    if (!d) return;
    d->bufferMode = (mode != 0) ? 1 : 0;
    if (buffer_ms > 0) d->bufferUs = (LONG)(buffer_ms * 1000);
}

extern "C" void basis_decoder_set_output_texture(basis_decoder_t* d, void* native_texture, int w, int h) {
    /* Windows uses D3D11/12 CreateExternalTexture (no Mali crash there), so the
     * AccessTexture path is not needed. Accept the call for ABI uniformity. */
    (void)d; (void)native_texture; (void)w; (void)h;
}

extern "C" int basis_decoder_get_debug(basis_decoder_t* d, char* buf, int size) {
    if (!d || !buf || size <= 0) return 0;
    return snprintf(buf, (size_t)size,
                    "blit=%ld copy=%ld render=%ld nodue=%ld acq=%ld lag=%ldms buf=%ldms mode=%d ttff=%ldms | acfg=%d aout=%ld asr=%d",
                    (long)d->dbg_blit, (long)d->dbg_copy, (long)d->dbg_render, (long)d->dbg_nodue, (long)d->dbg_acqfail,
                    (long)d->dbg_lagms, (long)(d->bufferUs / 1000), (int)d->bufferMode, (long)d->ttffMs,
                    d->aconfigured ? 1 : 0, (long)d->dbg_aout, d->asr);
}
