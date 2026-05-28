// Decoded video frame handed from a source/decoder to the renderer.
// Mutable class so future versions can be pooled without changing call sites.
public sealed class BasisVideoFrame
{
    public long PresentationTimeUs;
    public int Width;
    public int Height;
    public BasisVideoFrameFormat Format;
    public byte[] Data;
    public int DataLength;
}
