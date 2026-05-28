// Provides a media-time clock that something else (typically an audio sink) is
// driving. BasisAVSyncClock falls back to wall-clock pacing when no source is
// wired, but prefers this source when one is present and reports HasMediaTime.
//
// This is the audio-master sync pattern: video presentation tracks the time of
// the audio samples that have actually been demanded by the audio system, so
// video stays aligned with audio regardless of source/network/loop drift.
public interface IBasisMediaClockSource
{
    bool HasMediaTime { get; }
    long CurrentMediaTimeUs { get; }
}
