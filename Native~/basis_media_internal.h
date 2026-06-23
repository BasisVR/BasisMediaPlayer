/*
 * basis_media_internal.h — shared types between the engine core, the protocol
 * demuxers, and the platform decode/present backends.
 *
 * Layering:
 *   basis_media_core.c          engine lifecycle, demux thread, state, dispatch
 *   protocol/ sources           URL/IO/bitstream + RTSP/RTMP/TS/MP4 demuxers
 *                               (push elementary streams into a basis_media_sink)
 *   windows/ + android/         basis_decoder: OS decode -> GPU texture + PCM ring
 *
 * Demuxers never touch the platform decoder directly; they push into a
 * basis_media_sink whose callbacks the engine implements (forwarding to the
 * active basis_decoder). That keeps the protocol code portable and unit-testable.
 */

#ifndef BASIS_MEDIA_INTERNAL_H
#define BASIS_MEDIA_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "basis_media_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Elementary codec identifiers used across the sink boundary. */
typedef enum basis_codec {
    BASIS_CODEC_NONE = 0,
    BASIS_CODEC_H264 = 1,
    BASIS_CODEC_H265 = 2,
    BASIS_CODEC_AAC  = 10,
    BASIS_CODEC_LPCM = 11   /* Blu-ray HDMV LPCM (TS stream_type 0x80) */
} basis_codec_t;

/* Sink the demuxers push into. All callbacks are invoked from the demux thread.
 * `user` is the owning basis_media_engine. Implementations must be fast and must
 * not block the demux thread for long. */
typedef struct basis_media_sink {
    void* user;

    /* Called once per elementary video track when the codec/config is first known.
     * `extradata` is codec config: for H.264 the SPS/PPS (Annex B or avcC — the
     * decoder accepts either), for H.265 the VPS/SPS/PPS. May be NULL/0 when the
     * config is inline in the access units instead. */
    void (*on_video_format)(void* user, basis_codec_t codec,
                            const uint8_t* extradata, int extradata_len,
                            int width, int height);

    /* One coded video access unit in Annex B form (start-code separated NALUs),
     * with presentation timestamp in microseconds. key != 0 marks an IDR/keyframe. */
    void (*on_video_au)(void* user, const uint8_t* annexb, int len,
                        int64_t pts_us, int key);

    /* Called once when the audio codec/config is first known. For AAC, `asc` is
     * the AudioSpecificConfig (2+ bytes) when available. */
    void (*on_audio_format)(void* user, basis_codec_t codec,
                            int sample_rate, int channels,
                            const uint8_t* asc, int asc_len);

    /* One coded audio frame. For AAC this is a raw AAC frame (no ADTS header);
     * demuxers that produce ADTS strip it first. */
    void (*on_audio_frame)(void* user, const uint8_t* data, int len, int64_t pts_us);

    /* Lifecycle signals from the demuxer. */
    void (*on_state)(void* user, basis_media_state_t state);
    void (*on_error)(void* user, const char* message);
    void (*on_end_of_stream)(void* user);

    /* Demuxers poll this in their read loops; return 0 to unwind and exit. */
    int (*is_running)(void* user);
} basis_media_sink_t;

/* Generic blocking byte source for demuxers that read a continuous stream
 * (MPEG-TS / fMP4 over TCP or HTTP). Returns bytes read, 0 on EOF, <0 on error. */
typedef int (*basis_read_fn)(void* ctx, uint8_t* buf, int len);

/* ---- Platform decode/present backend (windows/ + android/) --------------- */

typedef struct basis_decoder basis_decoder_t;

/* Create/destroy the OS decoder bound to `engine` (used for logging/state). */
basis_decoder_t* basis_decoder_create(basis_media_engine_t* engine);
void             basis_decoder_destroy(basis_decoder_t* dec);

/* Configure tracks (called from the demux thread before the first submit). */
int basis_decoder_set_video_format(basis_decoder_t* dec, basis_codec_t codec,
                                   const uint8_t* extradata, int extradata_len,
                                   int width, int height);
int basis_decoder_set_audio_format(basis_decoder_t* dec, basis_codec_t codec,
                                   int sample_rate, int channels,
                                   const uint8_t* asc, int asc_len);

/* Submit coded data (demux thread). Annex B for video; raw AAC for audio. */
int basis_decoder_submit_video(basis_decoder_t* dec, const uint8_t* annexb, int len,
                               int64_t pts_us, int key);
int basis_decoder_submit_audio(basis_decoder_t* dec, const uint8_t* data, int len,
                               int64_t pts_us);

/* Optional: platforms whose OS layer can demux a URL itself (Android's
 * AMediaExtractor handles https TS/MP4 incl. TLS) take ownership here. Returns 1
 * if the decoder now owns the URL (the core must NOT run a demuxer and must not
 * call submit_*), 0 if the core should demux and feed submit_*(). The Windows
 * backend always returns 0. */
int basis_decoder_try_open_url(basis_decoder_t* dec, const char* url);

/* Render thread: publish the newest decoded frame into the Unity-visible texture,
 * and release GPU resources. */
int  basis_decoder_render_update(basis_decoder_t* dec);
void basis_decoder_render_release(basis_decoder_t* dec);

/* Accessors mirrored by the public ABI (any thread unless noted). */
void*    basis_decoder_get_texture(basis_decoder_t* dec, int* out_w, int* out_h);
uint64_t basis_decoder_get_frame_counter(basis_decoder_t* dec);
int      basis_decoder_get_video_size(basis_decoder_t* dec, int* out_w, int* out_h);
/* 0 = bottom-left origin (frame upright; sample as-is). 1 = top-left origin
 * (frame upside-down; consumer must flip V). Windows reports 1 when the D3D11
 * video processor can't mirror on this GPU; Vulkan always normalizes to 0. */
int      basis_decoder_get_frame_origin(basis_decoder_t* dec);
int64_t  basis_decoder_get_position_us(basis_decoder_t* dec);
int      basis_decoder_get_audio_format(basis_decoder_t* dec, int* out_rate, int* out_channels);
int      basis_decoder_read_audio(basis_decoder_t* dec, float* out, int max_floats); /* audio thread */
int      basis_decoder_get_debug(basis_decoder_t* dec, char* buf, int size); /* diagnostics */
void     basis_decoder_set_buffer(basis_decoder_t* dec, int mode, int buffer_ms); /* 0=fixed,1=dynamic */
void     basis_decoder_set_output_texture(basis_decoder_t* dec, void* native_texture, int w, int h); /* Android: Unity-owned dst */

/* ---- Engine internals shared with the platform backend ------------------ */

/* Set by the platform backend during UnityPluginLoad so decoders can create
 * resources on Unity's graphics device. Defined in basis_unity_plugin.cpp.
 * Targets: PC/VR = D3D11 (and D3D12 when the project runs DX12); Quest = Vulkan. */
typedef enum basis_gfx_api {
    BASIS_GFX_NONE   = 0,
    BASIS_GFX_D3D11  = 1,
    BASIS_GFX_D3D12  = 2,
    BASIS_GFX_VULKAN = 3,
    BASIS_GFX_GLES   = 4
} basis_gfx_api_t;

basis_gfx_api_t basis_gfx_get_api(void);

/* D3D: Unity's device. D3D12 also exposes the command queue for resource state. */
void* basis_gfx_get_d3d11_device(void);   /* ID3D11Device*  or NULL */
void* basis_gfx_get_d3d12_device(void);   /* ID3D12Device*  or NULL */
void* basis_gfx_get_d3d12_queue(void);    /* ID3D12CommandQueue* or NULL */

/* Vulkan: opaque pointer to the IUnityGraphicsVulkan instance, plus the raw
 * instance/device/physical-device/queue handles the AHB importer needs. The
 * returned values are VkInstance/VkDevice/VkPhysicalDevice/VkQueue as uintptr_t. */
void*    basis_gfx_get_unity_vulkan(void);
uint64_t basis_gfx_vk_instance(void);
uint64_t basis_gfx_vk_device(void);
uint64_t basis_gfx_vk_physical_device(void);
uint64_t basis_gfx_vk_graphics_queue(void);
uint32_t basis_gfx_vk_graphics_queue_family(void);

/* Vulkan: fetch Unity's currently-recording command buffer (so the YCbCr->RGBA
 * resolve runs inside Unity's frame, no separate submit/fence) and ensure we're
 * outside Unity's render pass. Writes Unity's current and "safe" (GPU-completed)
 * frame numbers for resource lifetime tracking. Returns VkCommandBuffer as
 * uintptr_t, or 0 if no buffer is available this call. Render thread only. */
uint64_t basis_gfx_vk_begin_record(uint64_t* out_current_frame, uint64_t* out_safe_frame);

/* Vulkan: ask Unity for the VkImage backing a C#-side Texture/RenderTexture
 * (its GetNativeTexturePtr()). Unity inserts pipeline barriers to transition
 * the resource to the requested layout/stage/access for the calling command
 * buffer. Returns 1 on success (out_image/out_layout/out_format/out_w/out_h
 * filled), 0 if unavailable. requested_layout uses raw VkImageLayout values
 * (e.g. VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL = 2). */
int basis_gfx_vk_access_texture(void* native_texture,
                                int requested_layout,
                                uint64_t* out_image,
                                int* out_layout,
                                int* out_format,
                                int* out_w,
                                int* out_h);

/* Thread-safe state/error helpers implemented in basis_media_core.c. */
void        basis_engine_set_state(basis_media_engine_t* engine, basis_media_state_t state);
void        basis_engine_set_error(basis_media_engine_t* engine, const char* message);
basis_decoder_t* basis_engine_get_decoder(basis_media_engine_t* engine);

/* Consulted by the platform backend: paused freezes video publishing and mutes
 * audio reads; running going to 0 tells decode/demux loops to unwind. */
int basis_engine_is_paused(basis_media_engine_t* engine);
int basis_engine_is_running(basis_media_engine_t* engine);

/* Non-zero when the source opened in paced (VOD) mode: the platform backend
 * presents on a fixed 1x-from-first-PTS clock instead of the live-edge clock. */
int basis_engine_is_paced(basis_media_engine_t* engine);

#ifdef __cplusplus
}
#endif

#endif /* BASIS_MEDIA_INTERNAL_H */
