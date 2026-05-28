using UnityEngine;

// End-to-end wiring for live OS-codec playback. Point a BasisMediaPlayer at a
// VRCDN (or any OS-decodable) live URL; the native engine decodes it with the
// platform hardware decoder straight into the player's zero-copy OutputTexture.
//
// Attach a BasisVideoMaterialOutput (renderer material) or BasisVideoDisplay
// (uGUI RawImage) on the same GameObject to display OutputTexture. For audio,
// add a BasisMediaPlayerAudio (with an AudioSource) on the same GameObject; the
// player feeds it PCM decoded natively.
//
// Per-platform protocol guidance (from https://panel.vrcdn.live/preview/<name>):
//   PC / VR (low latency) : rtspt://stream.vrcdn.live/live/<name>
//   Quest (Android)       : https://stream.vrcdn.live/live/<name>.live.ts   (MPEG-TS)
//   Alternatives          : rtmp://stream.vrcdn.live/live/<name>
//                           https://stream.vrcdn.live/live/<name>.live.mp4  (fMP4)
[RequireComponent(typeof(BasisMediaPlayer))]
public sealed class BasisMediaPlayerStreaming : MonoBehaviour
{
    [Header("Stream")]
    [Tooltip("Live URL to play when AutoSelectPerPlatform is off. RTSPT/RTMP/HTTPS-fMP4/HTTPS-TS are all accepted.")]
    public string StreamUrl = "rtspt://stream.vrcdn.live/live/vrcdn";

    [Tooltip("If true, pick PcUrl or QuestUrl automatically by build target instead of using StreamUrl. RTSPT is lowest latency on PC/VR; Quest pulls MPEG-TS over HTTPS.")]
    public bool AutoSelectPerPlatform = false;

    [Tooltip("URL used on desktop/standalone (and in the editor) when AutoSelectPerPlatform is on.")]
    public string PcUrl = "rtspt://stream.vrcdn.live/live/vrcdn";

    [Tooltip("URL used on Android/Quest when AutoSelectPerPlatform is on.")]
    public string QuestUrl = "https://stream.vrcdn.live/live/vrcdn.live.ts";

    [Header("Lifecycle")]
    [Tooltip("If true, the stream is loaded into the player on Start. Disable to call Configure() yourself.")]
    public bool ConfigureOnStart = true;

    private void Start()
    {
        if (ConfigureOnStart) Configure();
    }

    // Loads the resolved URL into the BasisMediaPlayer on this GameObject. The
    // player auto-plays if AutoPlayOnSourceAssigned is set (the default).
    public void Configure()
    {
        if (!TryGetComponent(out BasisMediaPlayer player))
        {
            BasisDebug.LogError("BasisMediaPlayerStreaming requires a BasisMediaPlayer on the same GameObject.", BasisDebug.LogTag.Video);
            return;
        }

        string url = ResolveUrl();
        if (string.IsNullOrEmpty(url))
        {
            BasisDebug.LogWarning("BasisMediaPlayerStreaming has no URL to load.", BasisDebug.LogTag.Video);
            return;
        }
        player.LoadUrl(url);
    }

    public string ResolveUrl()
    {
        if (!AutoSelectPerPlatform) return StreamUrl;
#if UNITY_ANDROID && !UNITY_EDITOR
        return QuestUrl;
#else
        return PcUrl;
#endif
    }
}
