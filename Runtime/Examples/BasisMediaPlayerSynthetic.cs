using UnityEngine;

// Drop on a GameObject that has a BasisMediaPlayer. Add a BasisVideoMaterialOutput
// (for renderer.material output) or a BasisVideoDisplay (for uGUI RawImage output)
// to display the synthetic pattern.
//
// On Start, hands the player a BasisSyntheticTestSource so a moving gradient
// appears with zero networking and zero native code involved.
[RequireComponent(typeof(BasisMediaPlayer))]
public sealed class BasisMediaPlayerSynthetic : MonoBehaviour
{
    [Header("Resolution")]
    [Min(16)] public int Width = 640;
    [Min(16)] public int Height = 360;
    [Min(1)]  public int FramesPerSecond = 30;

    [Header("Pattern")]
    public BasisSyntheticTestSource.Pattern PatternMode = BasisSyntheticTestSource.Pattern.Gradient;

    [Tooltip("Pixel size of each square when PatternMode is Checkerboard.")]
    [Min(1)] public int CheckerboardSquarePixels = 32;

    [Tooltip("Per-frame phase increment used by Gradient and Checkerboard patterns. Higher = faster animation.")]
    public int ColorCycleSpeed = 1;

    [Tooltip("RGBA fill used when PatternMode is SolidColor.")]
    public Color32 SolidColor = new Color32(64, 128, 192, 255);

    [Tooltip("Seed for the Noise pattern's pseudo-random byte generator. Reuse the same seed for reproducible noise.")]
    public int NoiseSeed = 0;

    [Header("Stream Control")]
    [Tooltip("If >0, the source raises OnEndOfStream after producing this many frames. Combine with BasisMediaPlayer.Loop to test restart paths.")]
    [Min(0)] public int EmitEndOfStreamAfterFrames = 0;

    private void Start()
    {
        if (!TryGetComponent(out BasisMediaPlayer player)) return;
        player.Renderer = new BasisRgba32FrameRenderer();
        player.Source = new BasisSyntheticTestSource(Width, Height, FramesPerSecond)
        {
            PatternMode = PatternMode,
            CheckerboardSquarePixels = CheckerboardSquarePixels,
            ColorCycleSpeed = ColorCycleSpeed,
            SolidR = SolidColor.r,
            SolidG = SolidColor.g,
            SolidB = SolidColor.b,
            SolidA = SolidColor.a,
            NoiseSeed = NoiseSeed,
            EmitEndOfStreamAfterFrames = EmitEndOfStreamAfterFrames,
        };
    }
}
