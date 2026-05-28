using System;
using System.Collections.Generic;

// Declarative description of "what to play" + "how to play it", consumed by
// BasisMediaPlayer.LoadSource. The player hands every network URL to the OS-codec
// engine (basis_media_native), which decodes it zero-copy into a GPU texture:
//
//   rtsp://, rtspt://     RTSP (rtspt = RTP interleaved over TCP — PC/VR low latency)
//   rtmp://, rtmps://     RTMP / RTMP-over-TLS
//   https://, http://     fragmented MP4 (.mp4) or MPEG-TS (.ts) over HTTP(S)
//
// The CPU IBasisFrameSource path (e.g. BasisSyntheticTestSource for tests) is
// entered only by assigning BasisMediaPlayer.Source directly. Disallowed schemes
// fall through to OnError with a clear message.
public sealed class BasisMediaSource
{
    public BasisMediaSource() { }

    public BasisMediaSource(string uri)
    {
        Uri = uri;
    }

    // URI of the media. Absolute URL or absolute local path (file://, /foo/bar,
    // C:\foo\bar). Relative paths are resolved against Application.streamingAssetsPath.
    public string Uri;

    // Optional headers passed to the underlying transport when the scheme
    // supports them (HTTP(S), WebSocket). Ignored by file/local sources.
    public Dictionary<string, string> Headers;

    // Per-source loop flag. Overridden by BasisMediaPlayer.Loop when assigned;
    // BasisMediaPlayer.Loop is the runtime-mutable knob.
    public bool Loop;

    // Initial playback rate (1.0 = real-time). Mirrors BasisMediaPlayer.PlaybackRate.
    public float PlaybackRate = 1f;

    // Initial volume (0..1). Mirrors BasisMediaPlayer.Volume.
    public float Volume = 1f;

    // Initial mute state. Mirrors BasisMediaPlayer.Mute.
    public bool Mute;

    // Where audio should flow. Currently always UnityAudioSource; held as the
    // enum so additional routings can be added without breaking call sites.
    public BasisAudioRouting AudioRouting = BasisAudioRouting.UnityAudioSource;

    // If set, the player will Seek to this position once OnReady fires.
    // Use TimeSpan.Zero to start at the beginning explicitly.
    public TimeSpan StartPosition = TimeSpan.Zero;

    // Connect/open timeout. Zero or negative means "use source default".
    public TimeSpan OpenTimeout = TimeSpan.Zero;

    // Free-form bag for source-specific options (e.g. preferred adaptive track,
    // hardware decoding hints). The player passes this through to the resolved
    // source; unknown keys are ignored.
    public Dictionary<string, object> Options;

    public static BasisMediaSource FromUrl(string url) => new BasisMediaSource(url);
    public static BasisMediaSource FromLocalPath(string path) => new BasisMediaSource(NormalizeLocalPath(path));

    private static string NormalizeLocalPath(string path)
    {
        if (string.IsNullOrEmpty(path)) return path;
        if (path.StartsWith("file://", StringComparison.OrdinalIgnoreCase)) return path;
        return "file://" + path.Replace('\\', '/');
    }
}
