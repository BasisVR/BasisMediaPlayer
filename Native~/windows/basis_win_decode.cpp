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

struct PcmRing {
    float* buf = nullptr;
    int cap = 0;     /* in floats */
    int head = 0, tail = 0;
    CRITICAL_SECTION cs;

    void init(int floats) { cap = floats; buf = (float*)malloc(sizeof(float) * cap); InitializeCriticalSection(&cs); }
    void destroy() { free(buf); buf = nullptr; DeleteCriticalSection(&cs); }
    void write(const float* s, int n) {
        EnterCriticalSection(&cs);
        for (int i = 0; i < n; ++i) {
            int nt = (tail + 1) % cap;
            if (nt == head) head = (head + 1) % cap; /* overwrite oldest */
            buf[tail] = s[i]; tail = nt;
        }
        LeaveCriticalSection(&cs);
    }
    int read(float* out, int n) {
        EnterCriticalSection(&cs);
        int got = 0;
        while (got < n && head != tail) { out[got++] = buf[head]; head = (head + 1) % cap; }
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
    int asr = 0, ach = 0, aobj = 2;
    int aBits = 32;                 /* output sample bits: 32=float, 16=PCM int */
    bool aconfigured = false;
    volatile LONG dbg_aout = 0;     /* AAC PCM outputs produced */
    PcmRing pcm;
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

    /* Use a type the decoder actually offers (Float preferred, else PCM16). */
    IMFMediaType* chosen = nullptr; int bits = 0;
    for (DWORD i = 0; ; ++i) {
        IMFMediaType* t = nullptr;
        if (FAILED(d->adec->GetOutputAvailableType(0, i, &t))) break;
        GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
        UINT32 b = 0; t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &b);
        if (sub == MFAudioFormat_Float) { if (chosen) chosen->Release(); chosen = t; bits = 32; break; }
        if (sub == MFAudioFormat_PCM && !chosen) { chosen = t; bits = (int)(b ? b : 16); }
        else t->Release();
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

        BYTE* p = nullptr; DWORD cur = 0;
        if (SUCCEEDED(mb->Lock(&p, nullptr, &cur)) && cur > 0) {
            if (d->aBits == 16) {
                int n = (int)(cur / 2);
                const int16_t* s16 = (const int16_t*)p;
                float tmp[4096];
                int off = 0;
                while (off < n) {
                    int c = n - off; if (c > 4096) c = 4096;
                    for (int i = 0; i < c; ++i) tmp[i] = s16[off + i] / 32768.0f;
                    d->pcm.write(tmp, c);
                    off += c;
                }
            } else {
                d->pcm.write((const float*)p, (int)(cur / sizeof(float)));
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
    if (!d || d->aconfigured || codec != BASIS_CODEC_AAC) return 0;
    d->asr = sample_rate; d->ach = channels;
    if (configure_audio_mft(d, asc, asc_len)) d->aconfigured = true;
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

extern "C" int basis_decoder_submit_audio(basis_decoder_t* d, const uint8_t* data, int len, int64_t pts_us) {
    if (!d || !d->adec || !data || len <= 0) return -1;
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
    if (buf < 40000) buf = 40000;
    if (buf > maxBuf) buf = maxBuf;
    d->bufferUs = (LONG)buf;

    /* Fast start: ramp the effective cushion from ~0 up to the target over the
     * first ~1.2s, so the first decoded frame is presented almost immediately
     * instead of waiting a full buffer behind live, then settle into the full
     * buffer. wallElapsed resets on a hard resync, so a rebuffer re-primes too. */
    int64_t wallElapsed = (int64_t)((nowq.QuadPart - d->wallStartQpc) * 1000000LL / freq);
    int64_t effBuf = (wallElapsed < 1200000) ? (buf * wallElapsed / 1200000) : buf;
    int64_t nowMedia = liveClock - effBuf;
    d->dbg_lagms = (LONG)((newest - nowMedia) / 1000);

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
extern "C" int64_t basis_decoder_get_position_us(basis_decoder_t* d) { return d ? d->lastPtsUs : -1; }
extern "C" int basis_decoder_get_audio_format(basis_decoder_t* d, int* r, int* c) {
    if (!d || !d->aconfigured) return -1;
    if (r) *r = d->asr ? d->asr : 48000;
    if (c) *c = d->ach ? d->ach : 2;
    return 0;
}
extern "C" int basis_decoder_read_audio(basis_decoder_t* d, float* out, int max_floats) {
    if (!d) return 0;
    int n = d->pcm.read(out, max_floats);
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
