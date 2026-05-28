// Decoded audio frame. Samples are interleaved float [-1, 1].
// SampleCount is per channel; total floats in Samples = SampleCount * ChannelCount.
public sealed class BasisAudioFrame
{
    public long PresentationTimeUs;
    public int SampleRate;
    public int ChannelCount;
    public float[] Samples;
    public int SampleCount;
}
