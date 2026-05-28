# Third-Party Notices

This package ships **no statically-linked third-party libraries**. The native
plugin `basis_media_native` decodes and presents using operating-system
frameworks only, so there are no extra copyright or license obligations from
bundled code.

| Binary shipped | Embeds | License obligation |
|---|---|---|
| `Plugins/Windows/x86_64/basis_media_native.dll` | (none — links OS frameworks) | none |
| `Plugins/Android/arm64-v8a/libbasis_media_native.so` | (none — links OS frameworks) | none |

## Operating-system frameworks used at runtime

These are part of the OS and are **not** redistributed by this package:

- **Windows** — Media Foundation (`mfplat`, `mfuuid`, AAC/H.264/H.265 decoder
  MFTs), Direct3D 11/12, DXGI, WinHTTP, Winsock.
- **Android** — NDK Media (`mediandk`: AMediaCodec/AMediaExtractor), Vulkan,
  `AHardwareBuffer`, Android NDK platform libraries.

## Build-time only (not shipped)

- **Unity PluginAPI headers** (`IUnityInterface.h`, `IUnityGraphics*.h`) are
  required to compile the native plugin glue. They are part of the Unity Editor
  (Unity Companion License) and are **not** included here — copy them from your
  Unity install at build time (see `Native~/unity/README.md`). The package
  `.gitignore` excludes them.

The previous pipeline's third-party components (libvpx for VP9, gorilla/websocket
and the Go runtime for the transcode server, and OpusSharp for audio) have all
been removed along with the VP9 transcode path.
