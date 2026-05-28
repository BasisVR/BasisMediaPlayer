public enum BasisVideoFrameFormat
{
    // 32-bit RGBA, 4 bytes per pixel, row-major, top-left origin.
    // Produced by test sources and any path that hands over already-decoded frames.
    Rgba32 = 0,

    // Planar YUV 4:2:0 (I420). Y plane is Width*Height bytes; U and V planes are
    // (Width/2)*(Height/2) each, concatenated after Y in the same buffer.
    // Native output of libvpx / dav1d, used once a real decoder is wired up.
    I420 = 1,
}
