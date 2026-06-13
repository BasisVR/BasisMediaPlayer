// A pull-based source of interleaved float PCM, read on the Unity audio thread.
//
// The OS-codec engine (BasisNativeVideoSource) decodes audio natively and exposes
// it as a ring. BasisMediaPlayerAudio sets this as its NativePcmSource and pulls
// from here on the audio thread, splitting/downmixing the interleaved samples
// across its output AudioSources.
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
