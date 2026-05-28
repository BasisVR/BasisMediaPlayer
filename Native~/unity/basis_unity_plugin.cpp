/*
 * basis_unity_plugin.cpp — Unity native plugin glue.
 *
 * Implements:
 *   - UnityPluginLoad/Unload + device-event callback to learn the graphics API
 *     and capture Unity's device handles (D3D11/D3D12/Vulkan) for the decoders.
 *   - basis_gfx_* accessors consumed by the platform backends.
 *   - basis_media_get_render_event_func(): the render-thread entry that dispatches
 *     BASIS_RENDER_UPDATE / BASIS_RENDER_RELEASE to basis_decoder_render_*.
 *
 * Requires Unity's PluginAPI headers (IUnityInterface.h, IUnityGraphics.h,
 * IUnityGraphicsD3D11.h, IUnityGraphicsD3D12.h, IUnityGraphicsVulkan.h). Copy them
 * from <UnityEditor>/Data/PluginAPI into this folder, or add that folder to the
 * compiler include path (see CMakeLists.txt / unity/README.md).
 */

#include "../basis_media_native.h"
#include "../basis_media_internal.h"

#include "IUnityInterface.h"
#include "IUnityGraphics.h"

#if defined(_WIN32)
  #include <d3d11.h>
  #include <d3d12.h>
  #include "IUnityGraphicsD3D11.h"
  #include "IUnityGraphicsD3D12.h"
#endif
#if defined(__ANDROID__)
  #include "IUnityGraphicsVulkan.h"
#endif

static IUnityInterfaces* s_unity = nullptr;
static IUnityGraphics*   s_graphics = nullptr;
static basis_gfx_api_t   s_api = BASIS_GFX_NONE;

static void* s_d3d11Device = nullptr;
static void* s_d3d12Device = nullptr;
static void* s_d3d12Queue  = nullptr;

static uint64_t s_vkInstance = 0, s_vkDevice = 0, s_vkPhys = 0, s_vkQueue = 0;
static uint32_t s_vkQueueFamily = 0;
static void*    s_unityVulkan = nullptr;

/* ---- basis_gfx_* accessors (internal header) ---------------------------- */

extern "C" basis_gfx_api_t basis_gfx_get_api(void) { return s_api; }
extern "C" void* basis_gfx_get_d3d11_device(void) { return s_d3d11Device; }
extern "C" void* basis_gfx_get_d3d12_device(void) { return s_d3d12Device; }
extern "C" void* basis_gfx_get_d3d12_queue(void) { return s_d3d12Queue; }
extern "C" void* basis_gfx_get_unity_vulkan(void) { return s_unityVulkan; }
extern "C" uint64_t basis_gfx_vk_instance(void) { return s_vkInstance; }
extern "C" uint64_t basis_gfx_vk_device(void) { return s_vkDevice; }
extern "C" uint64_t basis_gfx_vk_physical_device(void) { return s_vkPhys; }
extern "C" uint64_t basis_gfx_vk_graphics_queue(void) { return s_vkQueue; }
extern "C" uint32_t basis_gfx_vk_graphics_queue_family(void) { return s_vkQueueFamily; }

extern "C" uint64_t basis_gfx_vk_begin_record(uint64_t* out_current_frame, uint64_t* out_safe_frame) {
    if (out_current_frame) *out_current_frame = 0;
    if (out_safe_frame) *out_safe_frame = 0;
#if defined(__ANDROID__)
    if (!s_unityVulkan) return 0;
    IUnityGraphicsVulkan* vk = (IUnityGraphicsVulkan*)s_unityVulkan;
    /* Get outside any Unity render pass first (this may record), then grab the
     * recording state — resource-access calls would invalidate it, so we take it
     * last and only record our own commands afterward. */
    vk->EnsureOutsideRenderPass();
    UnityVulkanRecordingState rec;
    if (!vk->CommandRecordingState(&rec, kUnityVulkanGraphicsQueueAccess_DontCare)) return 0;
    if (out_current_frame) *out_current_frame = rec.currentFrameNumber;
    if (out_safe_frame) *out_safe_frame = rec.safeFrameNumber;
    return (uint64_t)(uintptr_t)rec.commandBuffer;
#else
    return 0;
#endif
}

extern "C" int basis_gfx_vk_access_texture(void* native_texture,
                                           int requested_layout,
                                           uint64_t* out_image,
                                           int* out_layout,
                                           int* out_format,
                                           int* out_w,
                                           int* out_h) {
    if (out_image) *out_image = 0;
    if (out_layout) *out_layout = 0;
    if (out_format) *out_format = 0;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
#if defined(__ANDROID__)
    if (!s_unityVulkan || !native_texture) return 0;
    IUnityGraphicsVulkan* vk = (IUnityGraphicsVulkan*)s_unityVulkan;
    /* AccessTexture inserts a pipeline barrier to transition the image to the
     * requested layout, and is documented to invalidate any previously-fetched
     * recording state — callers must re-query CommandRecordingState afterwards
     * if they need it (basis_android_vk's render path does AccessTexture first,
     * then re-queries via basis_gfx_vk_begin_record before recording). */
    UnityVulkanImage img = {};
    if (!vk->AccessTexture(native_texture,
                           UnityVulkanWholeImage,
                           (VkImageLayout)requested_layout,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           kUnityVulkanResourceAccess_PipelineBarrier,
                           &img))
        return 0;

    if (out_image) *out_image = (uint64_t)(uintptr_t)img.image;
    if (out_layout) *out_layout = (int)img.layout;
    if (out_format) *out_format = (int)img.format;
    if (out_w) *out_w = (int)img.extent.width;
    if (out_h) *out_h = (int)img.extent.height;
    return 1;
#else
    (void)native_texture; (void)requested_layout;
    return 0;
#endif
}

/* ---- device discovery --------------------------------------------------- */

static void capture_devices() {
    if (!s_graphics) return;
    UnityGfxRenderer r = s_graphics->GetRenderer();
    switch (r) {
#if defined(_WIN32)
        case kUnityGfxRendererD3D11: {
            s_api = BASIS_GFX_D3D11;
            IUnityGraphicsD3D11* d = s_unity->Get<IUnityGraphicsD3D11>();
            if (d) s_d3d11Device = d->GetDevice();
            break;
        }
        case kUnityGfxRendererD3D12: {
            s_api = BASIS_GFX_D3D12;
            // v2 exposes GetDevice() (enough for OpenSharedHandle). GetCommandQueue
            // is only on v5+ and requires a graphics-queue access opt-in via
            // Configure(); wire that if/when the D3D12 copy+fence path is finished.
            IUnityGraphicsD3D12v2* d = s_unity->Get<IUnityGraphicsD3D12v2>();
            if (d) s_d3d12Device = d->GetDevice();
            break;
        }
#endif
#if defined(__ANDROID__)
        case kUnityGfxRendererVulkan: {
            s_api = BASIS_GFX_VULKAN;
            IUnityGraphicsVulkan* vk = s_unity->Get<IUnityGraphicsVulkan>();
            if (vk) {
                s_unityVulkan = vk;
                UnityVulkanInstance inst = vk->Instance();
                s_vkInstance = (uint64_t)(uintptr_t)inst.instance;
                s_vkPhys     = (uint64_t)(uintptr_t)inst.physicalDevice;
                s_vkDevice   = (uint64_t)(uintptr_t)inst.device;
                s_vkQueue    = (uint64_t)(uintptr_t)inst.graphicsQueue;
                s_vkQueueFamily = inst.queueFamilyIndex;
            }
            break;
        }
#endif
        default: s_api = BASIS_GFX_NONE; break;
    }
}

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType type) {
    if (type == kUnityGfxDeviceEventInitialize) capture_devices();
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginLoad(IUnityInterfaces* interfaces) {
    s_unity = interfaces;
    s_graphics = interfaces->Get<IUnityGraphics>();
    if (s_graphics) {
        s_graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
    }
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
UnityPluginUnload() {
    if (s_graphics) s_graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    s_graphics = nullptr;
    s_unity = nullptr;
}

/* ---- render-thread entry ------------------------------------------------ */

static void BASIS_CALL OnRenderEvent(int event_id, void* data) {
    basis_media_engine_t* engine = (basis_media_engine_t*)data;
    basis_decoder_t* dec = basis_engine_get_decoder(engine);
    if (!dec) return;
    if (event_id == BASIS_RENDER_UPDATE) basis_decoder_render_update(dec);
    else if (event_id == BASIS_RENDER_RELEASE) basis_decoder_render_release(dec);
}

extern "C" BASIS_API basis_render_event_func BASIS_CALL basis_media_get_render_event_func(void) {
    return OnRenderEvent;
}
