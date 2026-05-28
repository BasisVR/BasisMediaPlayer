using System;

// Producer of decoded video/audio frames. Push-based: implementations fire events
// from whichever thread they read/decode on. Consumers (BasisMediaPlayer) marshal
// to the main thread for Unity API calls.
//
// Implementations: BasisSyntheticTestSource (CPU test pattern). Live OS-codec
// playback uses BasisNativeVideoSource (zero-copy GPU), which BasisMediaPlayer
// drives directly rather than through this interface.
public interface IBasisFrameSource : IDisposable
{
    event Action<BasisVideoFrame> OnVideoFrame;
    event Action<BasisAudioFrame> OnAudioFrame;
    event Action<Exception> OnError;
    event Action OnEndOfStream;

    // Fired once per Start() cycle, after the source has established that it
    // can deliver frames. For live sources this is "transport connected and
    // first packet observed"; for seekable sources this is the same signal as
    // IBasisSeekableFrameSource.OnPrepared (implementations may forward).
    event Action OnReady;

    // Fired whenever the source's reported video dimensions change (initial
    // resolution, mid-stream resize, adaptive variant switch). Argument order
    // is (width, height). Players cache the latest value and expose it as
    // BasisMediaPlayer.VideoSize.
    event Action<int, int> OnVideoSizeChanged;

    bool IsRunning { get; }

    void Start();
    void Stop();
}
