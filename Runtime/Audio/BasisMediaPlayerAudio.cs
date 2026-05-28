using System;
using System.Collections.Concurrent;
using UnityEngine;

// Drains BasisAudioFrames into a streaming AudioClip via a PCM read callback.
// Wire it to BasisMediaPlayer by subscribing Source.OnAudioFrame -> Enqueue.
// The callback runs on Unity's audio thread; Enqueue may be called from any thread.
public sealed class BasisMediaPlayerAudio : MonoBehaviour, IBasisMediaClockSource
{
    [Header("Output")]
    [Tooltip("AudioSource that will play the streaming clip. Leave empty to fall back to an AudioSource on this GameObject.")]
    public AudioSource TargetAudioSource;

    [Tooltip("If true, the streaming AudioClip is assigned to TargetAudioSource.clip on Awake. Disable to manage the clip yourself.")]
    public bool AssignClipOnAwake = true;

    [Tooltip("Name applied to the generated streaming AudioClip. Cosmetic; helps identify the clip in the profiler/audio mixer.")]
    public string ClipName = "BasisMediaPlayerAudio";

    [Header("Format")]
    [Tooltip("Sample rate of the active AudioClip. Auto-updated from the first incoming audio frame; the value here is the initial guess used before any frames arrive.")]
    public int SampleRate = 48000;

    [Tooltip("Channel count of the active AudioClip. Auto-updated from the first incoming audio frame; the value here is the initial guess used before any frames arrive.")]
    [Range(1, 2)] public int ChannelCount = 2;

    [Header("Buffering")]
    [Tooltip("Length of the internal streaming AudioClip in seconds. Unity fills the streaming clip in chunks ≈ this/12, so larger values produce coarser PCM callbacks and worse video sync. 1.0 keeps callbacks around 80 ms.")]
    [Min(0.1f)] public float ClipLengthSeconds = 1f;

    [Tooltip("Maximum number of pending audio frames before the overflow policy kicks in. Set to 0 for unbounded.")]
    [Min(0)] public int MaxQueuedFrames = 64;

    [Tooltip("If true, oldest frames are dropped once the queue is full. If false, new frames are dropped instead.")]
    public bool DropOldestOnOverflow = true;

    [Tooltip("After an underrun, the PCM callback stays silent until this many frames have queued. Prevents start-stop stutter when the source rate dips. 8 ≈ 160 ms at 20 ms/frame.")]
    [Min(1)] public int RebufferFrames = 8;

    [Header("Playback")]
    [Tooltip("If true, the AudioSource is played automatically when this component is enabled.")]
    public bool AutoPlayOnEnable = true;

    [Tooltip("If true, the AudioSource is stopped when this component is disabled.")]
    public bool StopOnDisable = true;

    [Tooltip("If true, the pending queue and partial frame are cleared on enable to avoid playing stale samples after a long pause.")]
    public bool ClearQueueOnEnable = false;

    [Tooltip("Sample-domain volume multiplier applied after decode. Use AudioSource.volume for spatial mixing; this exists to compensate for quiet/loud streams. Hard-capped at 2.0 at runtime so hostile content can't damage hearing or speakers.")]
    [Range(0f, 2f)] public float VolumeGain = 1f;

    [Tooltip("If true, decoded samples are zeroed before write. Use this to mute without stopping the AudioSource.")]
    public bool Mute = false;

    [Header("Status (read-only)")]
    [SerializeField] private int runtimeQueuedFrames;
    [SerializeField] private long runtimeConsumedSamples;
    [SerializeField] private long runtimeDroppedFrames;
    [SerializeField] private float runtimeLastPcmPeak;
    [SerializeField] private float runtimeLastPcmRms;

    public int QueuedFrameCount => pending.Count;
    public long ConsumedSampleCount => System.Threading.Interlocked.Read(ref runtimeConsumedSamples);
    public long DroppedFrameCount => runtimeDroppedFrames;
    public AudioSource ActiveAudioSource => audioSource;
    public AudioClip StreamingClip => clip;

    // When set, OnPcmRead pulls interleaved float PCM from here (the OS-codec
    // engine's native ring) instead of the decoded-frame queue. Assigned/cleared
    // by BasisMediaPlayer when a BasisNativeVideoSource becomes the backend.
    public IBasisPcmSource NativePcmSource { get; set; }
    public float LastPcmPeak => runtimeLastPcmPeak;
    public float LastPcmRms => runtimeLastPcmRms;

    // IBasisMediaClockSource — exposes media time driven by how many samples
    // the audio system has demanded since the first frame was actually played.
    // Sample counts come from the audio thread via Interlocked.Add; the read
    // side here is a relaxed Interlocked.Read so the value is consistent on
    // 32-bit platforms (no torn long reads).
    public bool HasMediaTime => hasFirstFramePts;
    public long CurrentMediaTimeUs
    {
        get
        {
            if (!hasFirstFramePts) return 0;
            long consumed = System.Threading.Interlocked.Read(ref runtimeConsumedSamples);
            int channels = Mathf.Max(1, ChannelCount);
            int rate = Mathf.Max(1, SampleRate);
            long samplesPerChannel = consumed / channels;
            long elapsedUs = samplesPerChannel * 1_000_000L / rate;
            long heardUs = firstFramePtsUs + elapsedUs - cachedAudioLatencyUs;
            return heardUs < firstFramePtsUs ? firstFramePtsUs : heardUs;
        }
    }

    public long AudioOutputLatencyUs => cachedAudioLatencyUs;

    private AudioSource audioSource;
    private AudioClip clip;
    private readonly ConcurrentQueue<BasisAudioFrame> pending = new ConcurrentQueue<BasisAudioFrame>();
    private float[] currentSamples;
    private int currentOffset;
    private int currentLimit;
    private long firstFramePtsUs;
    private volatile bool hasFirstFramePts;
    private volatile bool rebuffering = true;
    private volatile int pendingFormatRate;
    private volatile int pendingFormatChannels;
    private long cachedAudioLatencyUs;

    public void Enqueue(BasisAudioFrame frame)
    {
        if (frame?.Samples == null || frame.SampleCount <= 0) return;

        if (frame.SampleRate > 0 && frame.ChannelCount > 0 &&
            (frame.SampleRate != SampleRate || frame.ChannelCount != ChannelCount))
        {
            pendingFormatRate = frame.SampleRate;
            pendingFormatChannels = frame.ChannelCount;
        }

        int cap = MaxQueuedFrames;
        if (cap <= 0 || cap > BasisMediaPlayerSecurity.MaxQueuedAudioFramesCap) cap = BasisMediaPlayerSecurity.MaxQueuedAudioFramesCap;
        if (pending.Count >= cap)
        {
            if (DropOldestOnOverflow)
            {
                while (pending.Count >= cap && pending.TryDequeue(out _))
                {
                    runtimeDroppedFrames++;
                }
            }
            else
            {
                runtimeDroppedFrames++;
                return;
            }
        }

        pending.Enqueue(frame);
    }

    public void ClearQueue()
    {
        while (pending.TryDequeue(out _)) { }
        currentSamples = null;
        currentOffset = 0;
        currentLimit = 0;
        rebuffering = true;
    }

    // Requests a clip rebuild to the given format. Used by the native-engine path,
    // which decodes audio at the stream's rate and has no BasisAudioFrames to
    // infer the format from. Picked up by Update() (main thread).
    public void SetExpectedFormat(int sampleRate, int channels)
    {
        if (sampleRate <= 0 || channels <= 0) return;
        if (sampleRate == SampleRate && channels == ChannelCount) return;
        pendingFormatRate = sampleRate;
        pendingFormatChannels = Mathf.Clamp(channels, 1, 2);
    }

    // Wipes the media-time anchor and the consumed-sample counter so the next
    // arriving audio frame re-anchors this clock source. Called by BasisMediaPlayer
    // on Play() / Stop() to keep video presentation aligned across restarts.
    public void ResetSyncAnchor()
    {
        hasFirstFramePts = false;
        firstFramePtsUs = 0;
        rebuffering = true;
        System.Threading.Interlocked.Exchange(ref runtimeConsumedSamples, 0);
    }

    private void Awake()
    {
        if (TargetAudioSource != null)
        {
            audioSource = TargetAudioSource;
        }
        else if (!TryGetComponent(out audioSource))
        {
            audioSource = null;
        }
        if (audioSource == null)
        {
            BasisDebug.LogError(
                "BasisMediaPlayerAudio: no AudioSource assigned and none found on this GameObject. Assign TargetAudioSource or add an AudioSource component.",
                BasisDebug.LogTag.Video);
            return;
        }

        BuildClipForCurrentFormat();
    }

    private void BuildClipForCurrentFormat()
    {
        if (audioSource == null) return;

        try
        {
            AudioConfiguration cfg = AudioSettings.GetConfiguration();
            if (cfg.sampleRate > 0)
            {
                cachedAudioLatencyUs = (long)cfg.dspBufferSize * 1_000_000L / cfg.sampleRate;
            }
        }
        catch { cachedAudioLatencyUs = 0; }

        bool wasPlaying = audioSource.isPlaying;
        if (wasPlaying) audioSource.Stop();

        if (clip != null)
        {
            if (audioSource.clip == clip) audioSource.clip = null;
            Destroy(clip);
            clip = null;
        }

        int rate = Mathf.Max(8000, SampleRate);
        int channels = Mathf.Clamp(ChannelCount, 1, 2);
        float clipLen = Mathf.Min(ClipLengthSeconds, BasisMediaPlayerSecurity.ClipLengthSecondsCap);
        int lengthSamples = Mathf.Max(rate, Mathf.RoundToInt(rate * clipLen));
        clip = AudioClip.Create(
            name: string.IsNullOrEmpty(ClipName) ? "BasisMediaPlayerAudio" : ClipName,
            lengthSamples: lengthSamples,
            channels: channels,
            frequency: rate,
            stream: true,
            pcmreadercallback: OnPcmRead);

        if (AssignClipOnAwake)
        {
            audioSource.clip = clip;
            audioSource.loop = true;
            if (wasPlaying) audioSource.Play();
        }
    }

    private void OnEnable()
    {
        if (ClearQueueOnEnable) ClearQueue();
        if (AutoPlayOnEnable && audioSource != null && !audioSource.isPlaying) audioSource.Play();
    }

    private void OnDisable()
    {
        if (StopOnDisable && audioSource != null) audioSource.Stop();
    }

    private void Update()
    {
        runtimeQueuedFrames = pending.Count;

        int rate = pendingFormatRate;
        int ch = pendingFormatChannels;
        if (rate > 0 && ch > 0 && (rate != SampleRate || ch != ChannelCount))
        {
            SampleRate = rate;
            ChannelCount = ch;
            pendingFormatRate = 0;
            pendingFormatChannels = 0;
            BuildClipForCurrentFormat();
            ResetSyncAnchor();
            ClearQueue();
        }
    }

    private void OnDestroy()
    {
        if (clip != null)
        {
            Destroy(clip);
            clip = null;
        }
    }

    // Runs on the audio thread. Must not touch Unity APIs other than what's safe
    // from background threads. ConcurrentQueue access is fine.
    private void OnPcmRead(float[] data)
    {
        // OS-codec engine path: pull interleaved PCM straight from the native ring.
        var pcm = NativePcmSource;
        if (pcm != null)
        {
            int n = pcm.ReadPcm(data);
            if (n < 0) n = 0;
            else if (n > data.Length) n = data.Length;
            float g = Mute ? 0f : Mathf.Clamp(VolumeGain, 0f, 2f);
            if (g == 0f) Array.Clear(data, 0, n);
            else if (Math.Abs(g - 1f) > 0.0001f) { for (int i = 0; i < n; i++) data[i] *= g; }
            if (n < data.Length) Array.Clear(data, n, data.Length - n);
            System.Threading.Interlocked.Add(ref runtimeConsumedSamples, data.Length);
            float pk = 0f; double ss = 0;
            for (int i = 0; i < n; i++) { float v = data[i]; float a = v < 0f ? -v : v; if (a > pk) pk = a; ss += v * v; }
            runtimeLastPcmPeak = pk;
            runtimeLastPcmRms = n > 0 ? (float)Math.Sqrt(ss / n) : 0f;
            return;
        }

        int channels = ChannelCount;
        int needed = data.Length;
        int written = 0;
        float gain = Mute ? 0f : Mathf.Clamp(VolumeGain, 0f, 2f);
        float peak = 0f;
        double sumSq = 0;
        int summed = 0;

        if (rebuffering)
        {
            bool stillBuffering = pending.Count < Mathf.Max(1, RebufferFrames) && (currentSamples == null || currentOffset >= currentLimit);
            if (stillBuffering)
            {
                Array.Clear(data, 0, needed);
                System.Threading.Interlocked.Add(ref runtimeConsumedSamples, needed);
                runtimeLastPcmPeak = 0f;
                runtimeLastPcmRms = 0f;
                return;
            }
            rebuffering = false;
        }

        while (written < needed)
        {
            if (currentSamples != null && currentOffset < currentLimit)
            {
                int available = currentLimit - currentOffset;
                int take = Mathf.Min(needed - written, available);
                if (Math.Abs(gain - 1f) < 0.0001f)
                {
                    Array.Copy(currentSamples, currentOffset, data, written, take);
                }
                else if (gain == 0f)
                {
                    Array.Clear(data, written, take);
                }
                else
                {
                    for (int i = 0; i < take; i++) data[written + i] = currentSamples[currentOffset + i] * gain;
                }
                currentOffset += take;
                written += take;
                System.Threading.Interlocked.Add(ref runtimeConsumedSamples, take);
                continue;
            }

            if (!pending.TryDequeue(out var next))
            {
                int silence = needed - written;
                Array.Clear(data, written, silence);
                System.Threading.Interlocked.Add(ref runtimeConsumedSamples, silence);
                rebuffering = true;
                return;
            }

            currentSamples = next.Samples;
            currentOffset = 0;
            // Trust SampleCount as the source-of-truth length; cap to array
            // length to stay safe if a producer passes a padded buffer.
            currentLimit = Mathf.Min(next.SampleCount * channels, next.Samples.Length);

            // Anchor the media clock at the first frame the audio system
            // actually starts consuming. This is what video presentation will
            // be slaved to via BasisAVSyncClock.ExternalSource.
            if (!hasFirstFramePts)
            {
                firstFramePtsUs = next.PresentationTimeUs;
                hasFirstFramePts = true;
            }
        }

        for (int i = 0; i < written; i++)
        {
            float v = data[i];
            float a = v < 0f ? -v : v;
            if (a > peak) peak = a;
            sumSq += v * v;
            summed++;
        }
        runtimeLastPcmPeak = peak;
        runtimeLastPcmRms = summed > 0 ? (float)Math.Sqrt(sumSq / summed) : 0f;
    }
}
