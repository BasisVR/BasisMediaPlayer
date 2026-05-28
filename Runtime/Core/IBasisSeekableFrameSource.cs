using System;

// Frame source that can report total duration, current position, and accept
// Seek requests. File/URL-backed sources implement this; live streaming sources
// don't (use IBasisFrameSource directly).
//
// Position/Duration may report TimeSpan.MinValue while IsPrepared is false.
// Once OnPrepared has fired, both must return real values.
public interface IBasisSeekableFrameSource : IBasisFrameSource
{
    // True once the source has parsed enough header data to know its duration
    // and video dimensions, and is ready to begin emitting frames. Going from
    // false -> true raises OnPrepared exactly once per Start() cycle.
    bool IsPrepared { get; }

    // Total length of the media. TimeSpan.Zero for live streams that report
    // themselves as seekable but have no known end.
    TimeSpan Duration { get; }

    // Current playback position. For real-time sources this is the PTS of the
    // most recently emitted frame; for paused sources, the last value before pause.
    TimeSpan Position { get; }

    // Requests playback to jump to the given position. Implementations may
    // round to the nearest keyframe. OnSeekCompleted fires once the source has
    // resumed emitting frames at or near the requested position.
    void Seek(TimeSpan position);

    // Fires once when IsPrepared first becomes true. Re-armed by Start().
    event Action OnPrepared;

    // Fires after each Seek call once the new position is being emitted.
    // Argument is the actual position reached, which may differ from the
    // requested position due to keyframe alignment.
    event Action<TimeSpan> OnSeekCompleted;

    // Fires when the source restarts from the beginning under its own
    // looping mechanism (set via BasisMediaSource.Loop or equivalent). Sources
    // that defer looping to BasisMediaPlayer.Loop should not raise this — the
    // player raises its own OnLooped in that case.
    event Action OnLooped;

    // Fires when the source switches to a different adaptive bitrate variant.
    // Live/non-adaptive sources never raise this.
    event Action<BasisBitrateTrack> OnBitrateTrackChanged;
}
