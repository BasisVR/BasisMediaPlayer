// Describes one variant of an adaptive bitrate ladder (HLS/DASH/etc.).
// Emitted by IBasisSeekableFrameSource.OnBitrateTrackChanged when the source
// switches between variants. Live streaming sources that don't expose a ladder
// simply never raise the event.
public sealed class BasisBitrateTrack
{
    // Index in the source's track list; -1 if the source doesn't expose one.
    public int Index = -1;

    // Average bits-per-second of this variant. 0 if unknown.
    public int BitsPerSecond;

    // Native pixel dimensions of the variant. Zero if unknown (audio-only ladder).
    public int Width;
    public int Height;

    // Codec identifier as reported by the manifest (e.g. "avc1.640028", "vp09.00.50.08").
    public string Codec;

    // Free-form label from the manifest. May be null.
    public string Label;
}
