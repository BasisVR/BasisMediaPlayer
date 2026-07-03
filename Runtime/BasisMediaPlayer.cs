using System;
using System.Collections.Concurrent;
using System.IO;
using UnityEngine;
using UnityEngine.Rendering;

// Top-level facade. Wires an IBasisFrameSource (live streaming) or
// IBasisSeekableFrameSource (file / URL playback) to an IBasisFrameRenderer
// through a shared BasisAVSyncClock. Source events fire on arbitrary threads;
// the player queues frames and drains the queue on Update() so all Unity API
// calls stay on the main thread.
//
// Output model:
//   The player owns no Material. The renderer exposes a Texture
//   (BasisMediaPlayer.OutputTexture); consumers attach a BasisVideoMaterialOutput
//   (binds to a Renderer's material property) or a BasisVideoDisplay (binds to
//   a uGUI RawImage) on the same GameObject to actually display the texture.
//
// Typical wiring:
//   var player = gameObject.AddComponent<BasisMediaPlayer>();
//   gameObject.AddComponent<BasisVideoMaterialOutput>().TargetRenderer = quadRenderer;
//   player.LoadUrl("https://example.com/clip.mp4");

// High-level, display-friendly playback status, folded from the OS-codec engine
// state (BasisMediaEngineState) or the CPU source flags into one value the in-game
// Media Players panel shows. See BasisMediaPlayer.Status.
public enum BasisMediaPlayerStatus
{
    NoMedia = 0,    // nothing loaded
    Connecting,     // opening / connecting to the stream
    Buffering,      // connected, filling the buffer (no frame on screen yet)
    Ready,          // prepared, first frame available, not actively playing
    Playing,
    Paused,
    Stopped,        // had media, stopped by the user (or never started)
    Ended,          // reached end of stream
    Error,          // load/playback failed — see LastErrorMessage
}

[DisallowMultipleComponent]
public sealed class BasisMediaPlayer : MonoBehaviour
{
    public enum QueueOverflowPolicy
    {
        DropOldest = 0,
        DropNewest = 1,
    }

    [Header("Identity")]
    [Tooltip("Friendly name shown in the Media Players menu and diagnostics. Defaults to the GameObject name when empty.")]
    public string DisplayName;

    [Header("Buffering")]
    [Tooltip("Maximum queued frames before the overflow policy kicks in. Keeps memory bounded if the source outruns rendering. Defaults to ~1 second at 30 fps.")]
    [Min(1)] public int MaxQueueLength = 32;
    [Tooltip("DropOldest keeps fresh frames at the cost of skipping stale ones; DropNewest keeps the buffered history at the cost of ignoring new arrivals once full.")]
    public QueueOverflowPolicy OverflowPolicy = QueueOverflowPolicy.DropOldest;
    [Tooltip("Frames whose PTS is more than this many microseconds older than the clock are skipped without presenting. Set to 0 to always present the most recent ready frame. (CPU source path only.)")]
    [Min(0)] public long LateFrameSkipUs = 0;

    [Tooltip("OS-codec live path: Dynamic auto-tunes the jitter buffer (grows on stutter, shrinks when stable); Fixed holds BufferMilliseconds exactly. Lower = less latency, higher = smoother.")]
    public BasisVideoBufferMode BufferMode = BasisVideoBufferMode.Dynamic;
    [Tooltip("OS-codec live path: jitter buffer length in milliseconds (how far behind live video is presented). In Dynamic mode this is the starting value.")]
    [Min(0)] public int BufferMilliseconds = 120;

    [Header("Playback")]
    [Tooltip("If true, Play() is invoked automatically once a Source is assigned via LoadUrl/LoadLocalPath/LoadSource or the Source property.")]
    public bool AutoPlayOnSourceAssigned = true;
    [Tooltip("If true, Stop() is invoked when the component becomes disabled. Disable to keep the source running while the GameObject is inactive.")]
    public bool StopOnDisable = true;
    [Tooltip("Replays the source from the beginning when it raises OnEndOfStream. For seekable sources this triggers a seek-to-zero; for live streaming sources Play() is re-invoked.")]
    public bool Loop = false;
    [Tooltip("Delay before restarting a looped source, in seconds. Ignored when Loop is false.")]
    [Min(0f)] public float LoopRestartDelaySeconds = 0f;
    [Tooltip("Offset applied to media time before scheduling frames. Positive values present frames earlier (useful when audio is ahead), negative values delay them.")]
    public long PresentationOffsetUs = 0;

    [Header("Audio")]
    [Tooltip("Linear volume 0..1 applied to the audio path. Maps to BasisMediaPlayerAudio.VolumeGain.")]
    [Range(0f, 1f)] public float Volume = 1f;
    [Tooltip("If true, the audio path is silenced without stopping the source.")]
    public bool Mute = false;
    [Tooltip("Playback rate multiplier. 1.0 = real-time. Source backends that don't support rate changes ignore this.")]
    [Range(0.25f, 4f)] public float PlaybackRate = 1f;

    [Header("Captions")]
    [Tooltip("Show in-band closed captions (CEA-608) when the stream carries them. Client-side only — does not affect playback or sync. A BasisMediaCaptionOverlay (or your own UI) draws the cues; this toggles their visibility.")]
    [SerializeField] private bool captionsEnabled = false;
    [Tooltip("Caption text opacity (0..1). Client-side; applied by the caption overlay.")]
    [Range(0f, 1f)] [SerializeField] private float captionTextOpacity = 1f;
    [Tooltip("Caption background opacity (0..1). Client-side; applied by the caption overlay.")]
    [Range(0f, 1f)] [SerializeField] private float captionBackgroundOpacity = 0.5f;

    [Header("Sleep Timer")]
    [Tooltip("If > 0, the player calls Stop() automatically this many seconds after Play. 0 disables. Useful for fall-asleep-to-the-stream rooms.")]
    [Min(0f)] public float StopAfterSeconds = 0f;

    [Header("Capture")]
    [Tooltip("CaptureScreenshot flips rows before encoding to PNG. The native decoder emits a bottom-left origin frame, so the readback is already top-down for PNG and this defaults OFF; toggle if your platform reads back the other way.")]
    public bool FlipVerticallyForScreenshot = false;

    [Header("DVR")]
    [Tooltip("If true and the source is live, BufferMilliseconds is forced up to DvrWindowSeconds*1000 (capped at 60s) so the engine holds a rolling rewind window. Has no effect on seekable sources, which already support Seek to any position.")]
    public bool DvrEnabled = false;

    [Tooltip("Length of the live-rewind window in seconds. The engine retains this much decoded history; TrySeekBack(within this window) reverses playback. Costs proportional memory in the native ring; keep modest (5–30 s typical, 60 s cap).")]
    [Range(0f, 60f)] public float DvrWindowSeconds = 10f;

    [Header("Logging")]
    [Tooltip("If true, informational events (end-of-stream, restart, dropped frames) are logged. Errors are always logged regardless.")]
    public bool VerboseLogging = false;

    [Header("Status (read-only)")]
    [SerializeField] private bool runtimeIsPlaying;
    [SerializeField] private bool runtimeIsPaused;
    [SerializeField] private bool runtimeIsPrepared;
    [SerializeField] private int runtimeQueuedFrames;
    [SerializeField] private long runtimeCurrentMediaTimeUs;
    [SerializeField] private long runtimePresentedFrameCount;
    [SerializeField] private long runtimeOverflowDrops;
    [SerializeField] private long runtimeCatchUpSkips;
    [SerializeField] private long runtimeLateSkips;
    [SerializeField] private long runtimeFormatErrors;
    [SerializeField] private Vector2Int runtimeVideoSize;

    // Lifecycle events (raised on main thread from Update()).
    public event Action OnReady;
    public event Action OnStarted;
    public event Action OnFirstFrameReady;
    public event Action OnPaused;
    public event Action OnEnded;
    public event Action<Exception> OnError;
    public event Action OnLooped;
    public event Action<TimeSpan> OnSeekCompleted;
    public event Action<Texture> OnOutputTextureChanged;
    public event Action<BasisBitrateTrack> OnBitrateTrackChanged;
    public event Action<BasisAudioTrack> OnAudioTrackChanged;

    // The active in-band caption cue changed (CEA-608 CC1). Cue.Text is null when
    // the caption clears. Raised even while CaptionsEnabled is false so a display
    // can stay primed; honour CaptionsEnabled for visibility.
    public event Action<BasisCaptionCue> OnCaptionCueChanged;

    // CaptionsEnabled toggled. Client-side only — does not affect playback or sync.
    public event Action<bool> OnCaptionsEnabledChanged;

    // Caption styling (text/background opacity) changed. Client-side only.
    public event Action OnCaptionStyleChanged;

    // Diagnostic event; not in the public lifecycle list but kept for tooling
    // that wants per-frame callbacks.
    public event Action<BasisVideoFrame> OnFramePresented;

    public BasisAVSyncClock Clock { get; } = new BasisAVSyncClock();

    // Whether closed captions should be shown. Client-side preference; raises
    // OnCaptionsEnabledChanged so a caption overlay can show/hide immediately.
    public bool CaptionsEnabled
    {
        get => captionsEnabled;
        set
        {
            if (captionsEnabled == value) return;
            captionsEnabled = value;
            OnCaptionsEnabledChanged?.Invoke(value);
        }
    }

    // Caption text opacity, 0..1. Client-side; raises OnCaptionStyleChanged.
    public float CaptionTextOpacity
    {
        get => captionTextOpacity;
        set
        {
            value = Mathf.Clamp01(value);
            if (captionTextOpacity == value) return;
            captionTextOpacity = value;
            OnCaptionStyleChanged?.Invoke();
        }
    }

    // Caption background opacity, 0..1. Client-side; raises OnCaptionStyleChanged.
    public float CaptionBackgroundOpacity
    {
        get => captionBackgroundOpacity;
        set
        {
            value = Mathf.Clamp01(value);
            if (captionBackgroundOpacity == value) return;
            captionBackgroundOpacity = value;
            OnCaptionStyleChanged?.Invoke();
        }
    }

    public IBasisFrameRenderer Renderer
    {
        get => _renderer ??= new BasisRgba32FrameRenderer();
        set
        {
            DetachRenderer();
            _renderer?.Dispose();
            _renderer = value;
            AttachRenderer();
            // Push the current output texture through the change event so
            // newly attached consumers can rebind without waiting for the
            // next resize.
            OnOutputTextureChanged?.Invoke(_renderer?.OutputTexture);
        }
    }

    // Setting Source transfers ownership to the player. The previous source is
    // stopped and disposed before the new one is attached. OnDestroy disposes
    // whichever source is current. Callers that need to keep a source past the
    // player's lifetime should not assign it here.
    public IBasisFrameSource Source
    {
        get => source;
        set
        {
            if (ReferenceEquals(source, value)) return;
            // Assigning a CPU frame source takes over from the OS-codec engine.
            TeardownNativeEngine();
            DetachSource();
            try { source?.Dispose(); } catch (Exception ex) { BasisDebug.LogError($"BasisMediaPlayer source dispose failed: {ex.Message}", BasisDebug.LogTag.Video); }
            source = value;
            seekableSource = value as IBasisSeekableFrameSource;
            ResetSourceState();
            AttachSource();
            if (AutoPlayOnSourceAssigned && source != null) Play();
        }
    }

    // When the OS-codec engine is active it owns a zero-copy external texture;
    // otherwise the CPU renderer's RenderTexture is the output.
    public Texture OutputTexture => nativeEngine != null ? nativeEngine.OutputTexture : _renderer?.OutputTexture;

    // Vertical orientation correction the active backend needs the consumer to
    // apply. True when the output frame arrives upside-down relative to Unity's UV
    // convention (e.g. a Windows GPU whose video processor can't mirror); the video
    // sinks XOR this into their own FlipVertically so a screen authored on one GPU
    // looks correct on every client. False on the CPU source path and whenever the
    // frame is already upright.
    public bool OutputFrameIsTopLeftOrigin => nativeEngine != null && nativeEngine.FrameIsTopLeftOrigin;

    public bool IsPlaying => runtimeIsPlaying;
    public bool IsPaused => runtimeIsPaused;
    public bool IsPrepared => runtimeIsPrepared;
    public Vector2Int VideoSize => runtimeVideoSize;

    // Last error surfaced through OnError (native engine failure, blocked URL,
    // audio-rate mismatch, …) or a pre-flight load failure. Null when none.
    // Cleared when a fresh source is loaded. Read by the Media Players panel so a
    // failed/selected player can show what went wrong.
    public string LastErrorMessage { get; private set; }

    // Display-friendly playback status, folded from the active backend. Errors that
    // are non-fatal to video (e.g. audio muted on a sample-rate mismatch) are left
    // out here and surfaced via LastErrorMessage so playback still reads "Playing".
    public BasisMediaPlayerStatus Status
    {
        get
        {
            if (nativeEngine != null)
            {
                switch (nativeEngine.State)
                {
                    case BasisMediaEngineState.Connecting: return BasisMediaPlayerStatus.Connecting;
                    case BasisMediaEngineState.Buffering: return BasisMediaPlayerStatus.Buffering;
                    case BasisMediaEngineState.Playing: return runtimeIsPaused ? BasisMediaPlayerStatus.Paused : BasisMediaPlayerStatus.Playing;
                    case BasisMediaEngineState.Paused: return BasisMediaPlayerStatus.Paused;
                    case BasisMediaEngineState.Ended: return BasisMediaPlayerStatus.Ended;
                    case BasisMediaEngineState.Error: return BasisMediaPlayerStatus.Error;
                    default: return runtimeIsPlaying ? BasisMediaPlayerStatus.Connecting : BasisMediaPlayerStatus.Stopped;
                }
            }

            if (source != null)
            {
                if (runtimeIsPaused) return BasisMediaPlayerStatus.Paused;
                if (runtimeIsPlaying) return runtimePresentedFrameCount > 0 ? BasisMediaPlayerStatus.Playing : BasisMediaPlayerStatus.Buffering;
                if (runtimeIsPrepared) return BasisMediaPlayerStatus.Ready;
                return BasisMediaPlayerStatus.Stopped;
            }

            // No backend attached: a load that failed before one could attach (blocked
            // URL, missing native lib, unresolvable page URL) leaves an error but no source.
            if (!string.IsNullOrEmpty(LastErrorMessage)) return BasisMediaPlayerStatus.Error;
            return BasisMediaPlayerStatus.NoMedia;
        }
    }

    public TimeSpan Duration => seekableSource != null && seekableSource.IsPrepared
        ? seekableSource.Duration
        : TimeSpan.Zero;

    public TimeSpan Position
    {
        get
        {
            if (nativeEngine != null)
            {
                long ns = nativeEngine.PositionUs;
                return ns > 0 ? TimeSpan.FromTicks(ns * 10L) : TimeSpan.Zero;
            }
            if (seekableSource != null && seekableSource.IsPrepared) return seekableSource.Position;
            long us = Clock.CurrentMediaTimeUs;
            return us > 0 ? TimeSpan.FromTicks(us * 10L) : TimeSpan.Zero;
        }
    }

    public int QueuedFrameCount => videoQueue.Count;
    public long PresentedFrameCount => runtimePresentedFrameCount;
    public long OverflowDropCount => runtimeOverflowDrops;
    public long CatchUpSkipCount => runtimeCatchUpSkips;
    public long LateSkipCount => runtimeLateSkips;
    public long FormatErrorCount => runtimeFormatErrors;
    public long DroppedFrameCount => runtimeOverflowDrops + runtimeLateSkips + runtimeFormatErrors;
    public BasisMediaPlayerAudio AudioComponent => audioComponent;
    public BasisMediaSource ActiveMediaSource => activeMediaSource;

    // Bumped on every LoadUrl. An async resolver (the yt-dlp integration) captures this
    // before resolving and skips its LoadSource if it changed meanwhile, so a slow resolve
    // of an earlier URL can't overwrite a newer load. Read-only to callers.
    public int LoadGeneration { get; private set; }

    public System.Collections.Generic.IReadOnlyList<BasisBitrateTrack> BitrateTracks =>
        nativeEngine != null ? nativeEngine.BitrateTracks : System.Array.Empty<BasisBitrateTrack>();
    public int SelectedBitrateIndex => nativeEngine != null ? nativeEngine.SelectedBitrateIndex : -1;

    public bool SelectBitrate(int index) => nativeEngine != null && nativeEngine.SelectBitrate(index);

    public System.Collections.Generic.IReadOnlyList<BasisAudioTrack> AudioTracks =>
        nativeEngine != null ? nativeEngine.AudioTracks : System.Array.Empty<BasisAudioTrack>();
    public int SelectedAudioTrackIndex => nativeEngine != null ? nativeEngine.SelectedAudioTrackIndex : -1;

    public bool SelectAudioTrack(int index) => nativeEngine != null && nativeEngine.SelectAudioTrack(index);

    public bool TrySeekBack(TimeSpan back)
    {
        if (back <= TimeSpan.Zero) return false;
        if (seekableSource != null && seekableSource.IsPrepared)
        {
            var target = seekableSource.Position - back;
            if (target < TimeSpan.Zero) target = TimeSpan.Zero;
            Seek(target);
            return true;
        }
        if (nativeEngine != null)
        {
            long backUs = (long)Math.Min(back.TotalMilliseconds, int.MaxValue) * 1000L;
            bool ok = nativeEngine.SeekBackUs(backUs);
            if (!ok && VerboseLogging) BasisDebug.LogWarning("BasisMediaPlayer.TrySeekBack: native engine does not expose basis_media_seek_back_us (rebuild native plugin to enable live DVR).", BasisDebug.LogTag.Video);
            return ok;
        }
        return false;
    }

    private int ResolvedBufferMilliseconds()
    {
        if (!DvrEnabled) return BufferMilliseconds;
        int dvrMs = Mathf.RoundToInt(Mathf.Clamp(DvrWindowSeconds, 0f, 60f) * 1000f);
        return Mathf.Max(BufferMilliseconds, dvrMs);
    }

    // Active OS-codec backend, or null when on the CPU IBasisFrameSource path.
    // Exposed for diagnostics/tooling.
    public BasisNativeVideoSource NativeEngine => nativeEngine;

    private IBasisFrameSource source;
    private IBasisSeekableFrameSource seekableSource;
    private IBasisFrameRenderer _renderer;
    private readonly ConcurrentQueue<BasisVideoFrame> videoQueue = new ConcurrentQueue<BasisVideoFrame>();
    private float pendingRestartTimer;
    private bool restartScheduled;
    private ulong restartBaselineFrames;
    private int restartRechecks;
    private const int RestartMaxRechecks = 5;
    private const float RestartRecheckIntervalSeconds = 1f;
    private BasisMediaPlayerAudio audioComponent;
    private long lastEnqueuedPtsUs;
    private BasisMediaSource activeMediaSource;
    private float sleepTimerRemainingSeconds;
    private bool sleepTimerArmed;

    // Live OS-codec path (RTSPT / RTMP / MPEG-TS / fMP4). Mutually exclusive with
    // the CPU IBasisFrameSource path (synthetic test source): whichever is
    // non-null is the active backend. Assigned by LoadUrl/LoadSource; the CPU
    // path is entered only by assigning Source directly.
    private BasisNativeVideoSource nativeEngine;
    private Texture lastEngineTexture;

    // Pending main-thread signals from worker threads.
    private volatile bool sourceReadyPending;
    private volatile bool sourceEndOfStreamPending;
    private volatile bool loopEventPending;
    private long pendingVideoSize;
    private long pendingSeekCompletedUs = long.MinValue;
    private BasisBitrateTrack pendingBitrateTrack;
    private BasisAudioTrack pendingAudioTrack;
    private BasisCaptionCue? pendingCaptionCue;
    private Exception pendingError;
    private bool firstFrameEmittedThisPlay;
    private bool audioRateMismatchReported;

    public long HeadFramePtsUs => videoQueue.TryPeek(out var head) ? head.PresentationTimeUs : -1;
    public long TailFramePtsUs => System.Threading.Interlocked.Read(ref lastEnqueuedPtsUs);

    private void Awake()
    {
        // If a BasisMediaPlayerAudio lives on the same GameObject, it becomes
        // the master media clock so video frames are scheduled against audio
        // playback instead of wall time. Falls back to wall-clock pacing
        // automatically when no audio component is present (e.g. video-only).
        TryGetComponent(out audioComponent);
        if (audioComponent != null) Clock.ExternalSource = audioComponent;
        ApplyAudioSettingsToComponent();
        BasisMediaPlayerRegistry.Add(this);
    }

    // Convenience wrapper: builds a BasisMediaSource and calls LoadSource.
    // Page URLs (YouTube/Twitch/…) are steered through an installed resolver (the
    // optional yt-dlp integration) here, so every entry point — networking, the in-game
    // UI, the streaming example — resolves them; a directly-playable URL loads straight
    // through, and with no resolver installed a page URL reports that rather than
    // silently failing to demux an HTML page. LoadSource is NOT routed (it receives
    // already-resolved or direct sources, e.g. the resolver's own output).
    public void LoadUrl(string url)
    {
        if (string.IsNullOrWhiteSpace(url))
        {
            BasisDebug.LogWarning("BasisMediaPlayer.LoadUrl called with empty URL.", BasisDebug.LogTag.Video);
            return;
        }
        // Default a missing scheme to https so a bare "www.example.com/…" routes and loads
        // as an absolute URL instead of being mis-read as a direct/transport source.
        url = BasisMediaUrlRouter.NormalizeUrl(url);
        LastErrorMessage = null;
        LoadGeneration++;
        if (BasisMediaUrlRouter.TryResolveAndLoad(this, url)) return;
        if (!BasisMediaUrlRouter.IsDirectlyPlayable(url))
        {
            // A missing optional resolver is expected graceful degradation, not a fault — warn.
            // Surfaced to LastErrorMessage too so the Media Players panel can explain why nothing played.
            LastErrorMessage = "This looks like a page URL (e.g. YouTube/Twitch). Playing it needs a media URL resolver package, and none is installed.";
            BasisDebug.LogWarning(
                $"BasisMediaPlayer: '{BasisMediaUrlRouter.Redact(url)}' looks like a page URL (e.g. YouTube/Twitch); no media URL " +
                "resolver package is installed to handle it, so it can't be played.",
                BasisDebug.LogTag.Video);
            return;
        }
        var media = BasisMediaSource.FromUrl(url);
        LoadSource(media);
    }

    // Convenience wrapper: builds a BasisMediaSource from an absolute or
    // streaming-assets-relative path and calls LoadSource.
    public void LoadLocalPath(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
        {
            BasisDebug.LogWarning("BasisMediaPlayer.LoadLocalPath called with empty path.", BasisDebug.LogTag.Video);
            return;
        }
        var media = BasisMediaSource.FromLocalPath(path);
        LoadSource(media);
    }

    public void Reload()
    {
        if (activeMediaSource == null)
        {
            return;
        }

        Clock.Reset();
        audioComponent?.ResetSyncAnchor();
        LoadSource(activeMediaSource);
    }

    // Resolves the descriptor to the OS-codec engine and starts it. All network
    // URLs (rtsp/rtspt/rtmp/rtmps/http/https) are decoded by basis_media_native
    // straight into a GPU texture. The CPU IBasisFrameSource path is reserved for
    // code that assigns Source directly (e.g. BasisSyntheticTestSource for tests).
    public void LoadSource(BasisMediaSource media)
    {
        if (media == null)
        {
            throw new ArgumentNullException(nameof(media));
        }

        LastErrorMessage = null;
        activeMediaSource = media;

        /* not needed anymore!
        // Under Wine/Proton, Media Foundation may be missing and initializing the
        // native plugin can hard-crash, so ask before touching it (once per session,
        // shared across players). On native Windows/Android/Quest this runs inline.
        if (BasisVideoProtonGate.RequiresGate)
        {
            BasisVideoProtonGate.Request(
                onAllowed: () => StartNativeEngineForSource(media),
                onDenied: () => HandleProtonDeclined(media));
            return;
        }
        */

        StartNativeEngineForSource(media);
    }

    private void StartNativeEngineForSource(BasisMediaSource media)
    {
        _ = StartNativeEngineForSourceAsync(media);
    }

    private async System.Threading.Tasks.Task StartNativeEngineForSourceAsync(BasisMediaSource media)
    {
        // The decision may resolve a frame or more later (dialog / DNS), so re-check
        // this is still the source we were asked to load before starting the engine.
        if (!ReferenceEquals(activeMediaSource, media)) return;

        try
        {
            if (string.IsNullOrEmpty(media.Uri))
                throw new ArgumentException("BasisMediaSource.Uri is required.", nameof(media));
            if (!BasisMediaPlayerSecurity.IsUrlAllowed(media.Uri, out string blockReason))
                throw new UnauthorizedAccessException($"BasisMediaPlayer refused to load '{media.Uri}': {blockReason}");
            // The separate audio stream is its own network fetch, so it passes the
            // same trust gate as the video URL.
            if (!string.IsNullOrEmpty(media.AudioUri) &&
                !BasisMediaPlayerSecurity.IsUrlAllowed(media.AudioUri, out string audioBlockReason))
                throw new UnauthorizedAccessException($"BasisMediaPlayer refused to load audio '{media.AudioUri}': {audioBlockReason}");

            string dnsReason = await BasisMediaPlayerSecurity.ValidateResolvedHostAsync(media.Uri);
            if (dnsReason == null && !string.IsNullOrEmpty(media.AudioUri))
                dnsReason = await BasisMediaPlayerSecurity.ValidateResolvedHostAsync(media.AudioUri);
            if (dnsReason != null)
                throw new UnauthorizedAccessException($"BasisMediaPlayer refused to load '{media.Uri}': {dnsReason}");

            if (!ReferenceEquals(activeMediaSource, media)) return;

            SetNativeEngine(new BasisNativeVideoSource(ResolveNativeUri(media), ResolveNativeAudioUri(media), media.Delivery));
        }
        catch (Exception ex)
        {
            if (ReferenceEquals(activeMediaSource, media)) HandleError(ex);
        }

        if (ReferenceEquals(activeMediaSource, media)) ApplyMediaSourceSettings(media);
    }

    // RIST exposes a receive-buffer depth that librist parses straight from the URL
    // query. Framework devs set it via Options["buffer"] (milliseconds); fold it in
    // here so the native side sees rist://host:port?...&buffer=<ms>. Non-RIST schemes
    // ignore Options["buffer"], so the URI is returned untouched for them.
    private static string ResolveNativeUri(BasisMediaSource media)
    {
        string uri = media.Uri;
        if (media.Options != null &&
            uri.StartsWith("rist://", StringComparison.OrdinalIgnoreCase) &&
            media.Options.TryGetValue("buffer", out object buffer) && buffer != null)
        {
            char sep = uri.IndexOf('?') >= 0 ? '&' : '?';
            uri += $"{sep}buffer={buffer}";
        }
        return uri;
    }

    // The optional separate audio-only stream (BasisMediaSource.AudioUri), returned
    // verbatim; null when the source is a single muxed stream. Split audio streams are
    // HTTP fMP4, so the RIST buffer folding in ResolveNativeUri doesn't apply here.
    private static string ResolveNativeAudioUri(BasisMediaSource media) => media.AudioUri;

    private void HandleProtonDeclined(BasisMediaSource media)
    {
        if (!ReferenceEquals(activeMediaSource, media)) return;
        BasisDebug.LogWarning(
            "BasisMediaPlayer: not loading on Proton/Wine (declined, or Media Foundation unavailable). No video will play.",
            BasisDebug.LogTag.Video);
        ApplyMediaSourceSettings(media);
    }

    private void ApplyMediaSourceSettings(BasisMediaSource media)
    {
        if (media == null) return;
        Loop = media.Loop;
        PlaybackRate = media.PlaybackRate;
        if (media.Volume.HasValue) Volume = media.Volume.Value;
        if (media.Mute.HasValue) Mute = media.Mute.Value;
        ApplyAudioSettingsToComponent();
    }

    public void Play()
    {
        if (nativeEngine != null)
        {
            runtimeIsPaused = false;
            restartScheduled = false;
            pendingRestartTimer = 0f;
            firstFrameEmittedThisPlay = false;
            pendingCaptionCue = null;
            if (!nativeEngine.IsRunning) nativeEngine.Start();
            else nativeEngine.Play();
            runtimeIsPlaying = nativeEngine.IsRunning;
            if (runtimeIsPlaying)
            {
                ArmSleepTimer();
                OnStarted?.Invoke();
            }
            return;
        }

        if (source == null)
        {
            BasisDebug.LogWarning("BasisMediaPlayer.Play called with no Source assigned.", BasisDebug.LogTag.Video);
            return;
        }
        Clock.Reset();
        audioComponent?.ResetSyncAnchor();
        DrainQueue();
        ResetCounters();
        runtimeIsPaused = false;
        restartScheduled = false;
        pendingRestartTimer = 0f;
        firstFrameEmittedThisPlay = false;
        if (!source.IsRunning) source.Start();
        runtimeIsPlaying = source.IsRunning;
        if (runtimeIsPlaying)
        {
            ArmSleepTimer();
            OnStarted?.Invoke();
        }
    }

    public void Stop()
    {
        sleepTimerArmed = false;
        if (nativeEngine != null)
        {
            nativeEngine.Stop();
            ClearCaptionDisplay();
            runtimeIsPlaying = false;
            runtimeIsPaused = false;
            restartScheduled = false;
            pendingRestartTimer = 0f;
            return;
        }
        source?.Stop();
        Clock.Reset();
        audioComponent?.ResetSyncAnchor();
        DrainQueue();
        runtimeIsPlaying = false;
        runtimeIsPaused = false;
        restartScheduled = false;
        pendingRestartTimer = 0f;
    }

    public void Pause()
    {
        if (!runtimeIsPlaying || runtimeIsPaused) return;
        runtimeIsPaused = true;
        if (nativeEngine != null) nativeEngine.Pause();
        OnPaused?.Invoke();
    }

    public void Resume()
    {
        if (!runtimeIsPlaying || !runtimeIsPaused) return;
        runtimeIsPaused = false;
        if (nativeEngine != null)
        {
            nativeEngine.Play();
            OnStarted?.Invoke();
            return;
        }
        Clock.Reset();
        OnStarted?.Invoke();
    }

    public void CaptureScreenshot(string relativePath = null, Action<string, Exception> onComplete = null)
    {
        var tex = OutputTexture;
        if (tex == null) { onComplete?.Invoke(null, new InvalidOperationException("No output texture yet.")); return; }
        if (tex.width <= 0 || tex.height <= 0) { onComplete?.Invoke(null, new InvalidOperationException("Output texture has zero dimensions.")); return; }

        if (string.IsNullOrEmpty(relativePath))
        {
            relativePath = $"Screenshots/basis_video_{DateTime.UtcNow:yyyyMMdd_HHmmss_fff}.png";
        }
        if (!BasisMediaPlayerSecurity.TrySandboxLogPath(relativePath, out string fullPath, out string reason))
        {
            onComplete?.Invoke(null, new UnauthorizedAccessException(reason));
            return;
        }

        try
        {
            string dir = Path.GetDirectoryName(fullPath);
            if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
        }
        catch (Exception ex) { onComplete?.Invoke(null, ex); return; }

        int w = tex.width, h = tex.height;
        bool isBgra = SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D11
                   || SystemInfo.graphicsDeviceType == GraphicsDeviceType.Direct3D12;

        AsyncGPUReadback.Request(tex, 0, request =>
        {
            if (request.hasError) { onComplete?.Invoke(null, new Exception("AsyncGPUReadback failed.")); return; }
            try
            {
                var data = request.GetData<byte>();
                byte[] bytes = new byte[data.Length];
                data.CopyTo(bytes);
                if (isBgra) SwizzleBgraToRgba(bytes);
                if (FlipVerticallyForScreenshot) FlipRowsRgba32(bytes, w, h);
                var managed = new Texture2D(w, h, TextureFormat.RGBA32, false, false);
                managed.LoadRawTextureData(bytes);
                byte[] png = managed.EncodeToPNG();
                UnityEngine.Object.Destroy(managed);
                File.WriteAllBytes(fullPath, png);
                if (VerboseLogging) BasisDebug.Log($"BasisMediaPlayer wrote screenshot {fullPath} ({png.Length} bytes).", BasisDebug.LogTag.Video);
                onComplete?.Invoke(fullPath, null);
            }
            catch (Exception ex) { onComplete?.Invoke(null, ex); }
        });
    }

    private static void SwizzleBgraToRgba(byte[] rgba)
    {
        for (int i = 0; i + 3 < rgba.Length; i += 4) { byte b = rgba[i]; rgba[i] = rgba[i + 2]; rgba[i + 2] = b; }
    }

    private static void FlipRowsRgba32(byte[] data, int width, int height)
    {
        int stride = width * 4;
        var tmp = new byte[stride];
        for (int y = 0; y < height / 2; y++)
        {
            int top = y * stride;
            int bot = (height - 1 - y) * stride;
            Buffer.BlockCopy(data, top, tmp, 0, stride);
            Buffer.BlockCopy(data, bot, data, top, stride);
            Buffer.BlockCopy(tmp, 0, data, bot, stride);
        }
    }

    private void ArmSleepTimer()
    {
        if (StopAfterSeconds <= 0f) { sleepTimerArmed = false; return; }
        sleepTimerRemainingSeconds = StopAfterSeconds;
        sleepTimerArmed = true;
    }

    private void TickSleepTimer()
    {
        if (!sleepTimerArmed || runtimeIsPaused || !runtimeIsPlaying) return;
        sleepTimerRemainingSeconds -= Time.unscaledDeltaTime;
        if (sleepTimerRemainingSeconds > 0f) return;
        sleepTimerArmed = false;
        if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer sleep timer elapsed; stopping.", BasisDebug.LogTag.Video);
        Stop();
    }

    public void TogglePause()
    {
        if (runtimeIsPaused) Resume();
        else Pause();
    }

    // Jumps to the given position. Only meaningful for seekable sources; live
    // streaming sources throw NotSupportedException.
    public void Seek(TimeSpan position)
    {
        if (seekableSource == null)
        {
            throw new NotSupportedException(
                "BasisMediaPlayer.Seek requires a source that implements IBasisSeekableFrameSource. " +
                "The current source is live/non-seekable.");
        }
        DrainQueue();
        Clock.Reset();
        audioComponent?.ResetSyncAnchor();
        seekableSource.Seek(position);
    }

    private void OnValidate()
    {
        ApplyAudioSettingsToComponent();
        // Live-tune the buffer when changed in the inspector during play.
        if (Application.isPlaying) nativeEngine?.SetBuffer(BufferMode, ResolvedBufferMilliseconds());
    }

    private void OnDisable()
    {
        if (StopOnDisable) Stop();
    }

    private void OnDestroy()
    {
        BasisMediaPlayerRegistry.Remove(this);
        TeardownNativeEngine();
        DetachSource();
        try { source?.Dispose(); } catch { }
        source = null;
        seekableSource = null;
        DetachRenderer();
        _renderer?.Dispose();
        _renderer = null;
    }

    private void AttachSource()
    {
        if (source == null) return;
        source.OnVideoFrame += HandleVideoFrame;
        source.OnError += HandleError;
        source.OnEndOfStream += HandleEndOfStream;
        source.OnReady += HandleSourceReady;
        source.OnVideoSizeChanged += HandleVideoSizeChanged;
        if (seekableSource != null)
        {
            seekableSource.OnPrepared += HandleSourceReady;
            seekableSource.OnSeekCompleted += HandleSeekCompleted;
            seekableSource.OnLooped += HandleSourceLooped;
            seekableSource.OnBitrateTrackChanged += HandleBitrateTrackChanged;
        }
    }

    private void DetachSource()
    {
        if (source == null) return;
        source.OnVideoFrame -= HandleVideoFrame;
        source.OnError -= HandleError;
        source.OnEndOfStream -= HandleEndOfStream;
        source.OnReady -= HandleSourceReady;
        source.OnVideoSizeChanged -= HandleVideoSizeChanged;
        if (seekableSource != null)
        {
            seekableSource.OnPrepared -= HandleSourceReady;
            seekableSource.OnSeekCompleted -= HandleSeekCompleted;
            seekableSource.OnLooped -= HandleSourceLooped;
            seekableSource.OnBitrateTrackChanged -= HandleBitrateTrackChanged;
        }
    }

    private void SetNativeEngine(BasisNativeVideoSource engine)
    {
        // Drop any CPU source first; the engine becomes the active backend.
        if (source != null)
        {
            DetachSource();
            try { source.Dispose(); } catch (Exception ex) { BasisDebug.LogError($"BasisMediaPlayer source dispose failed: {ex.Message}", BasisDebug.LogTag.Video); }
            source = null;
            seekableSource = null;
        }
        TeardownNativeEngine();

        nativeEngine = engine;
        ResetSourceState();
        ResetCounters();
        firstFrameEmittedThisPlay = false;
        if (nativeEngine != null)
        {
            nativeEngine.OnReady += HandleSourceReady;
            nativeEngine.OnVideoSizeChanged += HandleVideoSizeChanged;
            nativeEngine.OnEndOfStream += HandleEndOfStream;
            nativeEngine.OnError += HandleError;
            nativeEngine.OnBitrateTrackChanged += HandleBitrateTrackChanged;
            nativeEngine.OnAudioTrackChanged += HandleAudioTrackChanged;
            nativeEngine.OnCaptionCueChanged += HandleCaptionCueChanged;
        }
        RouteNativePcmSource(nativeEngine);
        if (nativeEngine != null) nativeEngine.SetBuffer(BufferMode, ResolvedBufferMilliseconds());

        // Push the (currently null) external texture so consumers rebind once a
        // frame lands; the real texture arrives via OnOutputTextureChanged in Update.
        OnOutputTextureChanged?.Invoke(OutputTexture);

        if (AutoPlayOnSourceAssigned && nativeEngine != null) Play();
    }

    private void TeardownNativeEngine()
    {
        if (nativeEngine == null) return;
        nativeEngine.OnReady -= HandleSourceReady;
        nativeEngine.OnVideoSizeChanged -= HandleVideoSizeChanged;
        nativeEngine.OnEndOfStream -= HandleEndOfStream;
        nativeEngine.OnError -= HandleError;
        nativeEngine.OnBitrateTrackChanged -= HandleBitrateTrackChanged;
        nativeEngine.OnAudioTrackChanged -= HandleAudioTrackChanged;
        nativeEngine.OnCaptionCueChanged -= HandleCaptionCueChanged;
        if (audioComponent != null && ReferenceEquals(audioComponent.NativePcmSource, nativeEngine))
            audioComponent.NativePcmSource = null;
        try { nativeEngine.Dispose(); }
        catch (Exception ex) { BasisDebug.LogError($"BasisMediaPlayer native engine dispose failed: {ex.Message}", BasisDebug.LogTag.Video); }
        nativeEngine = null;
        lastEngineTexture = null;
        ClearCaptionDisplay();
    }

    private void AttachRenderer()
    {
        if (_renderer == null) return;
        _renderer.OnOutputTextureChanged += HandleOutputTextureChanged;
    }

    private void DetachRenderer()
    {
        if (_renderer == null) return;
        _renderer.OnOutputTextureChanged -= HandleOutputTextureChanged;
    }

    private void ResetSourceState()
    {
        runtimeIsPrepared = false;
        runtimeVideoSize = Vector2Int.zero;
        sourceReadyPending = false;
        sourceEndOfStreamPending = false;
        loopEventPending = false;
        pendingSeekCompletedUs = long.MinValue;
        pendingBitrateTrack = null;
        pendingAudioTrack = null;
        pendingCaptionCue = null;
        pendingError = null;
        LastErrorMessage = null;
        pendingVideoSize = 0;
        audioRateMismatchReported = false;
    }

    private void ResetCounters()
    {
        runtimePresentedFrameCount = 0;
        runtimeOverflowDrops = 0;
        runtimeCatchUpSkips = 0;
        runtimeLateSkips = 0;
        runtimeFormatErrors = 0;
        System.Threading.Interlocked.Exchange(ref lastEnqueuedPtsUs, 0);
    }

    private void ApplyAudioSettingsToComponent()
    {
        if (audioComponent != null) { audioComponent.VolumeGain = Volume; audioComponent.Mute = Mute; }
        if (nativeEngine != null) RouteNativePcmSource(nativeEngine);
    }

    private void RouteNativePcmSource(IBasisPcmSource pcm)
    {
        if (audioComponent != null) audioComponent.NativePcmSource = pcm;
    }

    private void HandleVideoFrame(BasisVideoFrame frame)
    {
        if (frame == null) return;
        int cap = MaxQueueLength > BasisMediaPlayerSecurity.MaxQueueLengthCap ? BasisMediaPlayerSecurity.MaxQueueLengthCap : MaxQueueLength;
        if (videoQueue.Count >= cap)
        {
            if (OverflowPolicy == QueueOverflowPolicy.DropNewest)
            {
                runtimeOverflowDrops++;
                BasisVideoFrameBufferPool.Return(frame.Data);
                frame.Data = null;
                if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer dropped newest frame; queue full.", BasisDebug.LogTag.Video);
                return;
            }
            while (videoQueue.Count >= cap && videoQueue.TryDequeue(out var stale))
            {
                runtimeOverflowDrops++;
                BasisVideoFrameBufferPool.Return(stale.Data);
                stale.Data = null;
            }
        }
        videoQueue.Enqueue(frame);
        System.Threading.Interlocked.Exchange(ref lastEnqueuedPtsUs, frame.PresentationTimeUs);
    }

    // The following Handle* methods run on whichever thread the source uses.
    // They stash a pending flag/value and let Update() raise events on the
    // Unity main thread.

    private void HandleError(Exception ex)
    {
        BasisDebug.LogError($"BasisMediaPlayer source error: {ex.Message}", BasisDebug.LogTag.Video);
        pendingError = ex;
    }

    private void HandleEndOfStream()
    {
        if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer reached end of stream.", BasisDebug.LogTag.Video);
        sourceEndOfStreamPending = true;
    }

    private void HandleSourceReady()
    {
        sourceReadyPending = true;
    }

    private void HandleVideoSizeChanged(int width, int height)
    {
        long packed = ((long)(uint)width << 32) | (uint)height;
        System.Threading.Interlocked.Exchange(ref pendingVideoSize, packed);
    }

    private void HandleSeekCompleted(TimeSpan position)
    {
        System.Threading.Interlocked.Exchange(ref pendingSeekCompletedUs, position.Ticks / 10L);
    }

    private void HandleSourceLooped()
    {
        // For source-driven looping (BasisMediaSource.Loop honored by native
        // backend), the source raises OnLooped; we forward on main thread.
        // For player-driven looping (Loop = true on this component), the
        // restart-scheduled path raises OnLooped after Play().
        sourceEndOfStreamPending = false;
        loopEventPending = true;
    }

    private void HandleBitrateTrackChanged(BasisBitrateTrack track)
    {
        pendingBitrateTrack = track;
    }

    private void HandleAudioTrackChanged(BasisAudioTrack track)
    {
        pendingAudioTrack = track;
    }

    private void HandleCaptionCueChanged(BasisCaptionCue cue)
    {
        pendingCaptionCue = cue;
    }

    // Drops any caption currently on screen. The engine only emits a clear cue when
    // a stream actively clears one, so stopping or swapping while a caption is visible
    // would otherwise leave the old text up until the next stream pushes its own cue.
    private void ClearCaptionDisplay()
    {
        pendingCaptionCue = null;
        OnCaptionCueChanged?.Invoke(new BasisCaptionCue(null, 0, 0));
    }

    private void HandleOutputTextureChanged(Texture texture)
    {
        // Renderer can fire from blit code paths, but Graphics.Blit is main-thread only,
        // so this comes in on the main thread already; forward directly.
        OnOutputTextureChanged?.Invoke(texture);
    }

    private void Update()
    {
        TickSleepTimer();

        // OS-codec engine path: drive the render-thread texture update, surface
        // ready/size/EOS/error events, then mirror status. No CPU frame queue.
        if (nativeEngine != null)
        {
            nativeEngine.Pump(VerboseLogging);
            DrainPendingEvents();
            PollNativeEngineStatus();
            HandleRestartTimer();
            return;
        }

        DrainPendingEvents();
        HandleRestartTimer();

        runtimeQueuedFrames = videoQueue.Count;
        runtimeCurrentMediaTimeUs = Clock.CurrentMediaTimeUs;

        if (runtimeIsPaused) return;

        // Anchor the clock to the first available frame's PTS, so wall-clock and
        // media-clock origins line up regardless of when Play() was called relative
        // to the source's first emit.
        if (!Clock.IsStarted)
        {
            if (!videoQueue.TryPeek(out var firstFrame)) return;
            Clock.StartAt(firstFrame.PresentationTimeUs);
        }

        // Walk forward until the next queued frame is in the future. The last frame
        // whose PTS is <= now becomes the one we actually render. Earlier frames
        // are skipped, which is what we want when rendering falls behind.
        long now = Clock.CurrentMediaTimeUs + PresentationOffsetUs;
        BasisVideoFrame toPresent = null;
        while (videoQueue.TryPeek(out var head) && head.PresentationTimeUs <= now)
        {
            if (toPresent != null)
            {
                runtimeCatchUpSkips++;
                BasisVideoFrameBufferPool.Return(toPresent.Data);
                toPresent.Data = null;
            }
            videoQueue.TryDequeue(out toPresent);
        }

        if (toPresent != null)
        {
            if (LateFrameSkipUs > 0 && now - toPresent.PresentationTimeUs > LateFrameSkipUs)
            {
                runtimeLateSkips++;
                BasisVideoFrameBufferPool.Return(toPresent.Data);
                toPresent.Data = null;
                if (VerboseLogging) BasisDebug.Log($"BasisMediaPlayer skipped late frame ({(now - toPresent.PresentationTimeUs)} us behind).", BasisDebug.LogTag.Video);
                return;
            }
            PresentFrame(toPresent);
        }
    }

    private void HandleRestartTimer()
    {
        if (!restartScheduled) return;
        pendingRestartTimer -= Time.unscaledDeltaTime;
        if (pendingRestartTimer > 0f) return;

        // Native live path: the engine reconnects internally on a dropped stream,
        // so re-check before tearing it down. If it already recovered, cancel the
        // restart. If it's still (re)connecting, give it another interval rather
        // than killing a stream that's about to come back. Only reconnect from
        // scratch once it's genuinely dead or we've waited long enough.
        if (nativeEngine != null && activeMediaSource != null)
        {
            var st = nativeEngine.State;
            bool framesFlowing = nativeEngine.DecodedFrameCount > restartBaselineFrames;

            if (nativeEngine.IsRunning && st == BasisMediaEngineState.Playing && framesFlowing)
            {
                restartScheduled = false;
                restartRechecks = 0;
                if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer restart skipped; stream recovered on its own.", BasisDebug.LogTag.Video);
                return;
            }

            if ((st == BasisMediaEngineState.Connecting || st == BasisMediaEngineState.Buffering) && restartRechecks < RestartMaxRechecks)
            {
                restartRechecks++;
                pendingRestartTimer = RestartRecheckIntervalSeconds;
                if (VerboseLogging) BasisDebug.Log($"BasisMediaPlayer restart deferred; engine still {st} ({restartRechecks}/{RestartMaxRechecks}).", BasisDebug.LogTag.Video);
                return;
            }

            restartScheduled = false;
            restartRechecks = 0;
            if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer auto-restarting source (stream did not recover).", BasisDebug.LogTag.Video);
            LoadSource(activeMediaSource); // live streams can't seek-to-zero; reconnect from scratch
            OnLooped?.Invoke();
            return;
        }

        restartScheduled = false;
        if (VerboseLogging) BasisDebug.Log("BasisMediaPlayer auto-restarting source.", BasisDebug.LogTag.Video);
        if (seekableSource != null && seekableSource.IsPrepared)
        {
            seekableSource.Seek(TimeSpan.Zero);
            OnLooped?.Invoke();
        }
        else
        {
            Play();
            OnLooped?.Invoke();
        }
    }

    // Mirrors the OS-codec engine's state into the runtime fields and fires the
    // texture/first-frame events. Runs on the main thread from Update().
    private void PollNativeEngineStatus()
    {
        if (nativeEngine == null) return;

        runtimeCurrentMediaTimeUs = Math.Max(0, nativeEngine.PositionUs);

        if (nativeEngine.TryGetPcmFormat(out int sr, out int ch) && sr > 0 && ch > 0)
        {
            // The streaming clip is created at the source rate (BasisMediaPlayerAudio.Rebuild),
            // so the AudioSource resamples it to the output rate — a rate mismatch is noted
            // once but still builds the audio path rather than muting. Video is unaffected.
            int outputRate = AudioSettings.outputSampleRate;
            if (sr != outputRate && !audioRateMismatchReported)
            {
                audioRateMismatchReported = true;
                BasisDebug.LogWarning(
                    $"Audio source rate {sr} Hz differs from output {outputRate} Hz; relying on AudioSource resampling.",
                    BasisDebug.LogTag.Video);
            }
            audioComponent?.SetExpectedFormat(sr, ch);
        }

        Texture tex = nativeEngine.OutputTexture;
        if (!ReferenceEquals(tex, lastEngineTexture))
        {
            lastEngineTexture = tex;
            OnOutputTextureChanged?.Invoke(tex);
            if (tex != null && !firstFrameEmittedThisPlay)
            {
                firstFrameEmittedThisPlay = true;
                OnFirstFrameReady?.Invoke();
            }
        }

        switch (nativeEngine.State)
        {
            case BasisMediaEngineState.Error:
            case BasisMediaEngineState.Ended:
                runtimeIsPlaying = false;
                break;
            case BasisMediaEngineState.Playing:
            case BasisMediaEngineState.Buffering:
                runtimeIsPlaying = !runtimeIsPaused;
                break;
        }
    }

    // Records a failure message (drives the Error status and the Media Players panel) and
    // raises OnError. Main thread only.
    private void RaiseError(Exception ex)
    {
        LastErrorMessage = ex.Message;
        OnError?.Invoke(ex);
    }

    /// <summary>
    /// Reports a load failure from an external URL resolver. A resolver that takes ownership via
    /// <see cref="BasisMediaUrlRouter"/> resolves asynchronously, so the player can't observe the
    /// outcome itself — it calls this (on the main thread) when resolution fails, so the reason
    /// surfaces through <see cref="LastErrorMessage"/> and <see cref="OnError"/> instead of the
    /// player sitting silently with no media.
    /// </summary>
    public void ReportLoadError(Exception error)
    {
        if (error != null) RaiseError(error);
    }

    private void DrainPendingEvents()
    {
        if (pendingError != null)
        {
            var ex = pendingError;
            pendingError = null;
            RaiseError(ex);
        }

        long packedSize = System.Threading.Interlocked.Exchange(ref pendingVideoSize, 0);
        if (packedSize != 0)
        {
            int w = (int)(packedSize >> 32);
            int h = (int)(packedSize & 0xFFFFFFFF);
            runtimeVideoSize = new Vector2Int(w, h);
        }

        if (sourceReadyPending)
        {
            sourceReadyPending = false;
            runtimeIsPrepared = true;
            OnReady?.Invoke();
            // Apply any deferred BasisMediaSource start-position now that the
            // source can actually accept a seek.
            if (activeMediaSource != null && activeMediaSource.StartPosition > TimeSpan.Zero && seekableSource != null)
            {
                try { seekableSource.Seek(activeMediaSource.StartPosition); }
                catch (Exception ex) { RaiseError(ex); }
            }
        }

        if (loopEventPending)
        {
            loopEventPending = false;
            OnLooped?.Invoke();
        }

        long seekedTo = System.Threading.Interlocked.Exchange(ref pendingSeekCompletedUs, long.MinValue);
        if (seekedTo != long.MinValue)
        {
            OnSeekCompleted?.Invoke(TimeSpan.FromTicks(seekedTo * 10L));
        }

        if (pendingBitrateTrack != null)
        {
            var track = pendingBitrateTrack;
            pendingBitrateTrack = null;
            OnBitrateTrackChanged?.Invoke(track);
        }

        if (pendingAudioTrack != null)
        {
            var track = pendingAudioTrack;
            pendingAudioTrack = null;
            OnAudioTrackChanged?.Invoke(track);
        }

        if (pendingCaptionCue.HasValue)
        {
            var cue = pendingCaptionCue.Value;
            pendingCaptionCue = null;
            OnCaptionCueChanged?.Invoke(cue);
        }

        if (sourceEndOfStreamPending)
        {
            sourceEndOfStreamPending = false;
            OnEnded?.Invoke();
            if (Loop && (source != null || nativeEngine != null))
            {
                restartScheduled = true;
                pendingRestartTimer = LoopRestartDelaySeconds;
                restartBaselineFrames = nativeEngine != null ? nativeEngine.DecodedFrameCount : 0;
                restartRechecks = 0;
            }
        }
    }

    private void PresentFrame(BasisVideoFrame frame)
    {
        var r = Renderer;
        if (!r.SupportsFormat(frame.Format))
        {
            BasisDebug.LogWarning($"BasisMediaPlayer renderer does not support frame format {frame.Format}; frame dropped.", BasisDebug.LogTag.Video);
            runtimeFormatErrors++;
            BasisVideoFrameBufferPool.Return(frame.Data);
            frame.Data = null;
            return;
        }
        if (!r.Present(frame))
        {
            runtimeFormatErrors++;
            BasisVideoFrameBufferPool.Return(frame.Data);
            frame.Data = null;
            return;
        }
        runtimePresentedFrameCount++;
        if (!firstFrameEmittedThisPlay)
        {
            firstFrameEmittedThisPlay = true;
            OnFirstFrameReady?.Invoke();
        }
        OnFramePresented?.Invoke(frame);
        BasisVideoFrameBufferPool.Return(frame.Data);
        frame.Data = null;
    }

    private void DrainQueue()
    {
        while (videoQueue.TryDequeue(out var f))
        {
            BasisVideoFrameBufferPool.Return(f.Data);
            f.Data = null;
        }
    }
}
