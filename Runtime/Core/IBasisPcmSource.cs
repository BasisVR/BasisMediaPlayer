// A pull-based source of interleaved float PCM, read on the Unity audio thread.
//
// BasisMediaPlayerAudio normally drains decoded BasisAudioFrames from a queue,
// but the OS-codec engine (BasisNativeVideoSource) decodes audio natively and
// exposes it as a ring instead. When BasisMediaPlayerAudio.NativePcmSource is
// set, OnPcmRead pulls from here directly rather than from the frame queue.
public interface IBasisPcmSource
{
    // Stream audio format, once known. Returns false until the first audio frame
    // has been decoded; the sink stays silent until then.
    bool TryGetPcmFormat(out int sampleRate, out int channels);

    // Fill `buffer` with up to buffer.Length interleaved float samples and return
    // how many floats were written. The caller zero-fills the remainder. Must not
    // block and must be safe to call from the audio thread.
    int ReadPcm(float[] buffer);
}
