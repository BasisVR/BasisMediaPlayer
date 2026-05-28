using UnityEngine;

// Maps real-world elapsed time to media presentation time.
//
// Default behavior anchors on the first frame's PTS and advances via
// Time.realtimeSinceStartupAsDouble — good enough when there's no audio.
//
// When an IBasisMediaClockSource is wired (BasisMediaPlayer hooks up a
// BasisMediaPlayerAudio on the same GameObject automatically), CurrentMediaTimeUs
// follows the source whenever it reports HasMediaTime. That makes video
// presentation track audio playback instead of wall time — the audio-master
// pattern used by ffplay / VLC / GStreamer — which keeps A/V aligned across
// source PTS jitter, loop-seam drift, and audio hardware clock skew.
//
// Smooth-takeover: when the local wall clock has already started (because the
// first video frame arrived before audio anchored), and the external source
// later starts reporting HasMediaTime, we anchor the external delta at the
// local clock's current value instead of letting external's absolute time take
// over. That avoids a backward time jump (audio anchored at PTS 0 while local
// clock had walked forward ~100 ms during the audio buffer pre-roll), which
// would otherwise leave queued video frames "in the future" forever.
public sealed class BasisAVSyncClock
{
    private bool started;
    private double wallStart;
    private long mediaStartUs;

    private IBasisMediaClockSource externalSource;
    private bool externalAnchorTaken;
    private long externalAnchorMediaUs;
    private long localAtExternalAnchorUs;
    private long lastExternalSampleMediaUs;
    private double lastExternalSampleWallSecs;
    private long lastReportedMediaUs;

    private const double MaxWallInterpolationSecs = 0.5;

    // When set and reporting HasMediaTime, drives both CurrentMediaTimeUs and
    // IsStarted. Set to null to revert to wall-clock anchoring.
    public IBasisMediaClockSource ExternalSource
    {
        get => externalSource;
        set => externalSource = value;
    }

    public bool IsStarted
    {
        get
        {
            if (externalSource != null && externalSource.HasMediaTime) return true;
            return started;
        }
    }

    public long CurrentMediaTimeUs
    {
        get
        {
            long value;
            if (externalSource != null && externalSource.HasMediaTime)
            {
                long externalNow = externalSource.CurrentMediaTimeUs;
                double wallNow = Time.realtimeSinceStartupAsDouble;
                if (!externalAnchorTaken)
                {
                    // Pin the external delta at the local clock's current value
                    // if local has run; otherwise start clean at external's value.
                    externalAnchorMediaUs = externalNow;
                    localAtExternalAnchorUs = started ? ComputeLocalMediaTimeUs() : externalNow;
                    externalAnchorTaken = true;
                    lastExternalSampleMediaUs = externalNow;
                    lastExternalSampleWallSecs = wallNow;
                }
                else if (externalNow != lastExternalSampleMediaUs)
                {
                    lastExternalSampleMediaUs = externalNow;
                    lastExternalSampleWallSecs = wallNow;
                }

                long baseValue = localAtExternalAnchorUs + (externalNow - externalAnchorMediaUs);
                double wallDeltaSecs = wallNow - lastExternalSampleWallSecs;
                if (wallDeltaSecs < 0) wallDeltaSecs = 0;
                else if (wallDeltaSecs > MaxWallInterpolationSecs) wallDeltaSecs = MaxWallInterpolationSecs;
                value = baseValue + (long)(wallDeltaSecs * 1_000_000.0);
            }
            else if (started)
            {
                value = ComputeLocalMediaTimeUs();
            }
            else
            {
                return 0;
            }

            if (value < lastReportedMediaUs) value = lastReportedMediaUs;
            else lastReportedMediaUs = value;
            return value;
        }
    }

    public void StartAt(long mediaTimeUs)
    {
        wallStart = Time.realtimeSinceStartupAsDouble;
        mediaStartUs = mediaTimeUs;
        started = true;
    }

    public void Reset()
    {
        started = false;
        wallStart = 0;
        mediaStartUs = 0;
        externalAnchorTaken = false;
        externalAnchorMediaUs = 0;
        localAtExternalAnchorUs = 0;
        lastExternalSampleMediaUs = 0;
        lastExternalSampleWallSecs = 0;
        lastReportedMediaUs = 0;
        // ExternalSource is wired once by the player and survives Reset();
        // the source itself is expected to reset its own anchor independently
        // (BasisMediaPlayerAudio.ResetSyncAnchor).
    }

    private long ComputeLocalMediaTimeUs()
    {
        double elapsedSeconds = Time.realtimeSinceStartupAsDouble - wallStart;
        return mediaStartUs + (long)(elapsedSeconds * 1_000_000.0);
    }
}
