# Basis Media Player

Live and on-demand video for Basis, decoded with the **operating-system hardware
codecs** and presented **zero-copy** into a Unity texture. No transcode server, no
VP9, no `UnityEngine.Video.MediaPlayer`.

- **Windows (PC / VR)** — Media Foundation H.264/H.265 + AAC on a DXVA D3D11
  device; NV12 → BGRA via the D3D11 video processor into a texture Unity samples.
  Works on **D3D11** (primary) and **D3D12** (shared-handle interop).
- **Android (Quest)** — `AMediaCodec`/`AMediaExtractor`; decoded frames arrive as
  `AHardwareBuffer`s imported into **Vulkan** as a `VkImage` Unity samples.

## Supported URLs (VRCDN and friends)

| Scheme | Use | Example |
|---|---|---|
| `rtspt://` | PC/VR low latency (RTP interleaved over TCP) | `rtspt://stream.vrcdn.live/live/vrcdn` |
| `rtmp://`  | RTMP pull | `rtmp://stream.vrcdn.live/live/vrcdn` |
| `rist://`  | RIST live ingest (UDP, loss recovery + optional AES) | `rist://stream.example:5000?secret=KEY&aes-type=128` |
| `https://…​.mp4` | fragmented MP4 over HTTPS | `https://stream.vrcdn.live/live/vrcdn.live.mp4` |
| `https://…​.ts`  | MPEG-TS over HTTPS (Quest) | `https://stream.vrcdn.live/live/vrcdn.live.ts` |
| `https://…​.m3u8` | HLS / Low-Latency HLS (Windows) | `https://stream.example/live/index.m3u8` |

The protocol/demux core (RTSP/RTP, RTMP/FLV, MPEG-TS, fMP4) is portable C; the OS
backends only decode + present.

### HLS / Low-Latency HLS

`.m3u8` URLs are handled by `protocol/basis_hls.c`, which is **not** a demuxer: it
parses the playlist, selects one rendition, starts at the live edge, and stitches
the segments — and, for LL-HLS, the partial segments (`EXT-X-PART`) — into one byte
stream that the existing MPEG-TS / fMP4 demuxers consume. When the origin advertises
`EXT-X-SERVER-CONTROL:CAN-BLOCK-RELOAD` with parts, the client uses blocking
`_HLS_msn`/`_HLS_part` playlist reloads and rides parts to target roughly
`PART-HOLD-BACK` latency (~5 s). **The ~5 s target needs an LL-HLS origin** — against
a plain HLS origin you get its segment-bound latency, not 5 s.

Runs on **Windows** (WinHTTP fetch), **clear streams**, **single rendition**.
Android/Quest support is planned.

### RIST

`rist://` ingests a RIST stream — MPEG-TS over UDP via librist, with
packet-loss recovery and optional AES encryption. librist reads its connection
options straight from the URL query: `?secret=<key>&aes-type=128` (or `256`)
for encryption, and `?buffer=<ms>` to size the recovery buffer. The buffer can
also be set from C# via `BasisMediaSource.Options["buffer"]`, folded into the
URL automatically. The recovered transport stream feeds the same MPEG-TS
demuxer as the HTTP/TS path.

RIST is **opt-in at build time** — the default plugin links only OS frameworks.
Build with `-DBASIS_WITH_RIST=ON` against prebuilt librist (see *Building the
native plugin* below).

## Live vs on-demand

Every source is either **live** (presented at the live edge, lowest latency) or
**on-demand** (VOD — paced to real time, so a file that arrives faster than it plays
doesn't fast-forward). `BasisMediaSource.Delivery` selects which:

| `Delivery` | Behaviour |
|---|---|
| `Auto` (default) | Decided at open from the source — see below |
| `Live` | Force the live-edge clock |
| `OnDemand` | Force real-time pacing |

`Auto` reads the source: a non-HTTP transport (`rtsp`/`rtmp`/`rist`) is live; an HTTP
response with a known `Content-Length` and byte-range support, or an HLS playlist
carrying `EXT-X-ENDLIST`, is on-demand; an open-ended HTTP response is live. On-demand
throttles delivery and presents on a fixed 1× clock, with a compressed read-ahead
buffer absorbing bursty CDN delivery.

`LoadUrl(url)` uses `Auto`. For explicit control, load a `BasisMediaSource`:

```csharp
player.LoadSource(new BasisMediaSource { Uri = url, Delivery = BasisMediaDelivery.OnDemand });
```

The live jitter buffer is tunable via `BasisMediaPlayer.BufferMilliseconds` /
`BufferMode` (Fixed, or auto-tuning Dynamic — lower = less latency, higher = smoother).
On-demand currently presents on a fixed internal buffer; `BufferMilliseconds` applies
to the live path only.

## Split-stream (separate video + audio)

Adaptive sources often serve high-resolution video and audio as **separate** streams
(H.264 video-only + AAC audio-only). Set `BasisMediaSource.AudioUri` alongside `Uri`
and the engine runs a second demux thread feeding the same decoder, so both present in
sync on one clock:

```csharp
player.LoadSource(new BasisMediaSource {
    Uri = videoOnlyUrl, AudioUri = audioOnlyUrl, Delivery = BasisMediaDelivery.OnDemand,
});
```

A null `AudioUri` (the default) is an ordinary single muxed stream.

## Page URLs (optional resolver package)

The player opens **stream** URLs (the schemes above) directly. It does **not** itself
turn a **page** URL — a YouTube or Twitch watch page — into a stream. That resolution
is provided by a **separate, optional package** (a yt-dlp-based resolver) which
registers itself on `BasisMediaUrlRouter`; the player core has no dependency on it and
never references it.

**With the resolver package installed**, a URL field such as
`BasisMediaPlayerStreaming.StreamUrl` steers each URL automatically:

- A **directly-playable** URL — a transport scheme, or an HTTP URL whose path ends in a
  media extension (`.mp4`/`.m4s`/`.ts`/`.m2ts`/`.mts`/`.m3u8`) — loads directly.
- **Anything else** (an HTTP page URL with no media extension) is handed to the
  resolver, which turns it into the playable stream endpoint(s) and loads them.

**Without it**, the router is inert: every URL loads directly, so all the stream URLs
above keep working — but page URLs are no longer resolved, so **YouTube, Twitch and
similar links won't play** (loading one reports that the resolver package is needed,
rather than failing silently). Removing the package is a supported choice: you lose
common-site resolution and nothing else. (This only steers — it never blocks a URL;
host trust is enforced separately.)

> **Known gap.** The steering keys off the URL's form, so a **direct HTTP stream with
> no file extension** (e.g. `https://host/live/feed` with no `.ts`/`.mp4`) can't be
> told apart from a page URL: with the resolver installed it is sent to the resolver,
> which finds no extractable stream and reports an error — so playback fails rather
> than loading directly. Give direct HTTP streams a recognised
> extension, or use a transport scheme (`rtsp`/`rtmp`), to avoid this.

## Usage

```csharp
var player = gameObject.AddComponent<BasisMediaPlayer>();
gameObject.AddComponent<BasisVideoMaterialOutput>().TargetRenderer = quadRenderer;
player.LoadUrl("rtspt://stream.vrcdn.live/live/vrcdn"); // auto-plays
```

Or drop the `Prefabs/MediaPlayerStreaming` prefab in a scene and set the URL on
`BasisMediaPlayerStreaming` (it can auto-pick RTSPT on PC / MPEG-TS on Quest).
Add a `BasisMediaPlayerAudio` (+ `AudioSource`) for sound;
`BasisMediaPlayerNetworking` syncs URL/state across the room.

The CPU `IBasisFrameSource` path (e.g. `BasisSyntheticTestSource`) is still
available by assigning `player.Source` directly — useful for tests without a feed.

### Audio (stereo and multichannel)

Audio routes through a `BasisMediaPlayerAudio` on the player GameObject. List the
`AudioSource`s in `Outputs`, each carrying a `BasisMediaAudioChannel` that selects
what it plays — a single decoded channel, or a stereo downmix of the whole stream.
For stereo, use a single `Output` set to `Stereo` (the `Prefabs/MediaPlayerStreaming`
prefab); for surround, one `Output` per channel so a 5.1 / 7.1 mix (up to 8
channels) can be positioned speaker-by-speaker in the world (the
`Prefabs/MediaPlayerMultiChannelStreaming` prefab).

Channel ceiling depends on the source: **LPCM over MPEG-TS** carries a full 7.1
(8 channels); **AAC on Windows** decodes up to 5.1 (the Media Foundation
decoder's limit).

## Networked sync

`BasisMediaPlayerNetworking` keeps playback aligned across the room. It syncs the
**input URL** — the page URL you entered, not the resolved stream — plus play / pause /
stop. A page URL resolves to a per-client, expiring CDN URL that can't be shared, so each
client resolves the shared page URL itself; direct stream URLs travel verbatim. To keep a
shared load tight, the owner broadcasts a page URL up front so peers resolve it in parallel
rather than only after the owner is playing, and re-loading a URL (even the same one)
restarts every client together.

> **On-demand position sync is start-together, not catch-up.** Clients stay aligned by
> beginning the same source at the same time; there is no continuous re-syncing of an
> on-demand playhead. A client that joins late, or whose load lands later, starts from the
> beginning and will not match an already-running playhead until the next shared (re)load.
> This is a current limitation of the native decode backend, which exposes no absolute or
> forward seek (only relative rewind), so a behind client cannot be advanced to catch up.
> Live streams are unaffected — they converge to the live edge. Direct, seekable sources do
> apply position/pause on load.

## Building the native plugin

Source is under `Native~/`. By default it links **only OS frameworks** (no
third-party libs). The optional RIST transport (`-DBASIS_WITH_RIST=ON`)
statically links prebuilt librist (which vendors its own mbedTLS) from
`Native~/third_party/`. Build that archive with `Native~/build-librist.ps1`
(Windows) or `build-librist.sh` (Linux/Android), or download it from the
**media-native (RIST)** CI workflow's artifacts — see
`Native~/third_party/README.md`. Then add `-DBASIS_WITH_RIST=ON` to the cmake
configure step below. You also need Unity's PluginAPI headers — see
`Native~/unity/README.md`.

**Windows → `Plugins/Windows/x86_64/basis_media_native.dll`**
```
cmake -S Native~ -B Native~/build -A x64 -DUNITY_PLUGIN_API_DIR="<UnityEditor>/Editor/Data/PluginAPI"
cmake --build Native~/build --config Release
```

**Android (arm64, Vulkan) → `Plugins/Android/arm64-v8a/libbasis_media_native.so`**
```
cmake -S Native~ -B Native~/build-android \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DUNITY_PLUGIN_API_DIR=<UnityEditor>/Editor/Data/PluginAPI
cmake --build Native~/build-android --config Release
```

After building, set the plugin's platform/CPU in the Unity import settings and the
`Texture2D.CreateExternalTexture` format follows `SystemInfo.graphicsDeviceType`
(BGRA32 on D3D11/D3D12, RGBA32 on Vulkan) — handled in `BasisNativeVideoSource`.

## Status / iterate-here

This is a large native change validated only by build structure. Most likely to
need on-device iteration:

- **Android Vulkan** — the YCbCr→RGBA resolve pass (sampler ycbcr-conversion +
  fullscreen pipeline + Unity-queue coordination via `IUnityGraphicsVulkan`) is
  scaffolded but not finished; see the `TODO(on-device)` in
  `Native~/android/basis_android_vk.c`. The AHB import is implemented.
- **D3D12** — present opens the shared BGRA as an `ID3D12Resource`; cross-API GPU
  sync between the D3D11 video processor and the D3D12 sampler should use a shared
  fence — validate for tearing (see notes in `Native~/windows/basis_win_decode.cpp`).
- **RTMP** — handshake/AMF is minimal (simple handshake, no Digest auth, no rtmps).
  RTSPT and MPEG-TS are the primary, more-complete paths.
- **HEVC on Windows** needs the system HEVC decoder MFT (HEVC Video Extensions).
